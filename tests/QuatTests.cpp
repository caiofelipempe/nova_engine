// Rotations (nova/math/Quat.h): fixed-point quaternions, Euler angles in six orders, slerp / nlerp /
// rotateTowards, and rotations of TransformComponent and scene files.
// Run: ctest --preset ... or NovaQuatTests.exe
#include <nova/Nova.h>

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

bool sameRotation(const glm::dquat& a, const glm::dquat& b, f64 eps = 1e-6) {
    return std::abs(std::abs(glm::dot(a, b)) - 1.0) <= eps; // q and −q are the same rotation
}
bool sameRotation(const Quat& a, const Quat& b, f64 eps = 1e-6) { return sameRotation(a.toDouble(), b.toDouble(), eps); }
f64 degreesBetween(const Quat& a, const Quat& b) { return static_cast<f64>(angleBetween(a, b).raw()) / Angle::kTurn * 360.0; }

void glmInterop() {
    // Matches glm within the format's precision.
    const glm::quat g = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
    const Quat q = g; // quantized where assigned
    const glm::vec3 p(1.25f, -2.0f, 3.5f);
    CHECK(glm::length(glm::dvec3(g * p) - q.rotate(LengthVec(p)).toDouble()) < 1e-5);
    CHECK(glm::length(glm::dvec3(g * p) - (q * LengthVec(p)).toDouble()) < 1e-5);
    CHECK(sameRotation(glm::dquat(q.toGlm()), glm::dquat(g)));
    // Unnormalized and degenerate input.
    CHECK(Quat(glm::quat(2, 0, 0, 0)) == Quat::identity());
    CHECK(Quat(glm::quat(0, 0, 0, 0)) == Quat::identity());
    static_assert(sizeof(Quat) == 16 && sizeof(Quat16) == 8);
    // A compact rotation: the same turn, coarser.
    const Quat16 c = q.as<Q<1, 14>>();
    CHECK(sameRotation(c.toDouble(), q.toDouble(), 1e-4));
}

void axisAngle() {
    // Quarter turns are exact: the trigonometry is exact at multiples of 90°.
    const Quat z90 = Quat::aroundZ(Angle::quarter());
    CHECK(glm::length(z90.rotate(LengthVec(2.0, 0.0, 0.0)).toDouble() - glm::dvec3(0, 2, 0)) < 1e-5);
    CHECK(Quat::fromAxisAngle(UnitVec(0.0, 0.0, 1.0), Angle()) == Quat::identity());
    // Any axis length and format.
    CHECK(sameRotation(Quat::fromAxisAngle(ScaleVec(0.0, 3.0, 0.0), Angle::fromDegrees(30)), Quat::aroundY(Angle::fromDegrees(30))));
    CHECK(Quat::fromAxisAngle(ScaleVec(), Angle::fromDegrees(30)) == Quat::identity());
    // Half a turn and its wrap: 270° is −90° around the same axis.
    CHECK(sameRotation(Quat::aroundY(Angle::fromDegrees(270)), Quat::aroundY(Angle::fromDegrees(-90))));
    CHECK(glm::length(Quat::aroundY(Angle::half()).rotate(UnitVec(1.0, 0.0, 0.0)).toDouble() - glm::dvec3(-1, 0, 0)) < 1e-8);
    // Composition order as glm, inverse.
    const Quat a = Quat::aroundZ(Angle::fromDegrees(90)), b = glm::angleAxis(-1.1f, glm::vec3(0, 1, 0));
    const glm::vec3 p(1.25f, -2.0f, 3.5f);
    CHECK(glm::length((a * b).rotate(LengthVec(p)).toDouble() - glm::dvec3((a.toGlm() * b.toGlm()) * p)) < 1e-5);
    CHECK(glm::length(a.inverse().rotate(a.rotate(LengthVec(p))).toDouble() - glm::dvec3(p)) < 1e-5);
    // Local axes.
    CHECK(glm::length(Quat::aroundY(Angle::quarter()).forward().toDouble() - glm::dvec3(-1, 0, 0)) < 1e-8);
}

void unitLength() {
    // A long chain of small turns stays unit and close to the double reference.
    Quat acc;
    glm::dquat ref(1, 0, 0, 0);
    const Quat step = glm::angleAxis(0.01f, glm::normalize(glm::vec3(0.3f, 1, -0.2f)));
    for (int i = 0; i < 10000; ++i) {
        acc = (step * acc).normalized();
        ref = step.toDouble() * ref;
    }
    CHECK(std::abs(glm::length(acc.toDouble()) - 1.0) < 1e-8);
    CHECK(sameRotation(acc.toDouble(), glm::normalize(ref)));
    // renormalized(): free for unit quaternions, fixes drift.
    const Quat q = Quat::fromEulerDegrees({20, 40, 60});
    CHECK(q.renormalized() == q);
    const Quat drifted = Quat::fromComponents(q.w * 1.01, q.x * 1.01, q.y * 1.01, q.z * 1.01);
    CHECK(std::abs(glm::length(drifted.renormalized().toDouble()) - 1.0) < 1e-8);
    CHECK(Quat::fromComponents(Unit(), Unit(), Unit(), Unit()).normalized() == Quat::identity());
    // Rotating the largest vectors does not overflow (the length is kept).
    const Length big = fromRaw<Length>(std::numeric_limits<i32>::max()); // just under 8192 m
    const LengthVec far{big, zero<Length>(), zero<Length>()};
    CHECK(std::abs(toDouble(q.rotate(far).length()) - toDouble(big)) < 1e-3);
    // A million small turns of a transform stay unit length.
    TransformComponent t;
    for (int i = 0; i < 1'000'000; ++i) t.rotateLocal(Angle::fromRadians(1e-3), ScaleVec(0.3, 1.0, 0.2));
    CHECK(std::abs(glm::length(t.rotation.toDouble()) - 1.0) < 1e-6);
}

void euler() {
    const EulerOrder orders[] = {EulerOrder::XYZ, EulerOrder::XZY, EulerOrder::YXZ, EulerOrder::YZX, EulerOrder::ZXY, EulerOrder::ZYX};
    std::mt19937 rng(7);
    std::uniform_real_distribution<f64> any(-3.0, 3.0), middle(-1.4, 1.4);
    bool roundTrip = true, anglesBack = true;
    for (EulerOrder order : orders)
        for (int i = 0; i < 200; ++i) {
            // Angles in the canonical range: the middle rotation of the order within ±80°.
            glm::dvec3 e(any(rng), any(rng), any(rng));
            e[eulerOrderName(order)[1] - 'X'] = middle(rng);
            const Quat q = Quat::fromEulerRadians(e, order);
            const EulerAngles back = q.toEuler(order);
            roundTrip &= sameRotation(Quat::fromEuler(back, order), q);
            anglesBack &= glm::length(back.toDouble() - e) < 1e-6;
        }
    CHECK(roundTrip && anglesBack);

    // The old convention (glm::quat(vec3), used by older scene files) is ZYX.
    bool legacy = true;
    for (int i = 0; i < 100; ++i) {
        const glm::dvec3 e(any(rng), any(rng), any(rng));
        legacy &= sameRotation(Quat::fromEulerRadians(e, EulerOrder::ZYX).toDouble(), glm::dquat(e));
    }
    CHECK(legacy);

    // Known values: 90° yaw turns forward (−Z) to −X; the order matters once two axes are involved.
    CHECK(glm::length(Quat::fromEulerDegrees({0, 90, 0}).forward().toDouble() - glm::dvec3(-1, 0, 0)) < 1e-8);
    CHECK(!sameRotation(Quat::fromEulerDegrees({30, 60, 0}, EulerOrder::XYZ), Quat::fromEulerDegrees({30, 60, 0}, EulerOrder::YXZ)));
    // Yaw then pitch (YXZ): pitch is around the camera's own right axis, so forward keeps its heading.
    const glm::dvec3 f = Quat::fromEulerDegrees({-30, 90, 0}).forward().toDouble();
    CHECK(f.x < -0.8 && f.y < -0.4 && std::abs(f.z) < 1e-8);

    // Gimbal lock: pitch ±90° in YXZ; the angles still rebuild the same rotation.
    const Quat locked = Quat::fromEulerDegrees({90, 40, 25});
    const glm::dvec3 lockedBack = locked.toEulerDegrees();
    CHECK(std::abs(lockedBack.x - 90.0) < 1e-2 && lockedBack.z == 0.0);
    CHECK(sameRotation(Quat::fromEulerDegrees(lockedBack), locked, 1e-6));

    // TransformComponent: stored as a quaternion, viewed and set as Euler in any order.
    TransformComponent t;
    t.setEulerDegrees({10, 20, 30}, EulerOrder::ZXY);
    CHECK(glm::length(t.eulerDegrees(EulerOrder::ZXY) - glm::dvec3(10, 20, 30)) < 1e-6);
    t.rotation = Quat::identity();
    t.rotate(Angle::fromDegrees(90), UnitVec(0.0, 1.0, 0.0));
    t.rotateLocal(Angle::fromDegrees(90), UnitVec(1.0, 0.0, 0.0)); // then pitch up around its own X
    CHECK(glm::length(t.forward() - glm::vec3(0, 1, 0)) < 1e-5f);
    t.position = {0.0, 0.0, 0.0};
    t.lookAt(GlobalPosition(3.0, 0.0, -3.0));
    CHECK(glm::length(t.forward() - glm::normalize(glm::vec3(1, 0, -1))) < 1e-5f);
}

void interpolation() {
    // slerp: endpoints, constant angular speed, shortest arc whatever the sign.
    const Quat a = Quat::identity(), b = Quat::fromEulerDegrees({0, 120, 0});
    CHECK(sameRotation(slerp(a, b, Unit(0)), a) && sameRotation(slerp(a, b, Unit(1)), b));
    CHECK(std::abs(degreesBetween(a, slerp(a, b, Unit(0.25))) - 30.0) < 1e-5);
    CHECK(std::abs(degreesBetween(a, slerp(a, b, Unit(0.5))) - 60.0) < 1e-5);
    CHECK(sameRotation(slerp(a, -b, Unit(0.5)), slerp(a, b, Unit(0.5))));
    CHECK(std::abs(glm::length(slerp(a, b, Unit(0.37)).toDouble()) - 1.0) < 1e-8);
    // nlerp: same path and endpoints, unit length, not constant speed.
    CHECK(sameRotation(nlerp(a, b, Unit(1)), b) && std::abs(glm::length(nlerp(a, b, Unit(0.5)).toDouble()) - 1.0) < 1e-8);
    CHECK(sameRotation(nlerp(a, b, Unit(0.5)), slerp(a, b, Unit(0.5)))); // the midpoint coincides
    // angleBetween: [0, π], whatever the sign; π itself.
    CHECK(std::abs(degreesBetween(a, b) - 120.0) < 1e-5 && std::abs(degreesBetween(a, -b) - 120.0) < 1e-5);
    CHECK(angleBetween(a, Quat::aroundX(Angle::half())).raw() == Angle::half().raw());
    // rotateTowards: limited steps, then exactly the target.
    const Quat step = rotateTowards(a, b, Angle::fromDegrees(10));
    CHECK(std::abs(degreesBetween(a, step) - 10.0) < 1e-5);
    CHECK(rotateTowards(a, b, Angle::fromDegrees(170)) == b);
    TransformComponent t;
    t.rotateTowards(b, Angle::fromDegrees(10));
    CHECK(t.rotation == step);

    // Interpolating whole transforms: exact position, slerped rotation, mixed scale.
    TransformComponent from, to;
    from.position = GlobalPosition::fromUnits({7, 0, 0}, LengthUnit::Kilometer);
    to.position = from.position + GlobalOffset::fromMeters(glm::dvec3(10.0, 0.0, 0.002));
    to.rotation = b;
    to.scale = glm::vec3(3.0f);
    const TransformComponent mid = TransformComponent::lerp(from, to, Unit(0.5));
    const GlobalOffset half = (mid.position - from.position) - GlobalOffset::fromMeters(glm::dvec3(5.0, 0.0, 0.001));
    CHECK(std::abs(half.x) <= 1 && half.y == 0 && std::abs(half.z) <= 1); // within one step (rounding of the half)
    CHECK(std::abs(degreesBetween(mid.rotation, a) - 60.0) < 1e-5);
    CHECK(mid.scale == ScaleVec(2.0, 2.0, 2.0));
}

void sceneFiles() {
    // Quaternions as [x, y, z, w], bit for bit; older files (Euler radians) still load.
    Scene scene;
    Entity e = scene.createEntity("Rot");
    e.transform().setEulerDegrees({15, -70, 5});
    const std::string json = serializeScene(scene);
    Scene back;
    CHECK(deserializeScene(back, json));
    CHECK(back.findEntity("Rot").transform().rotation == e.transform().rotation);
    Scene old;
    const char* legacyFile = R"({"format": "nova.scene", "version": 1, "entities": [
        {"components": {"Name": {"name": "Old"}, "Transform": {"position": [0, 0, 0], "rotation": [0.3, -1.2, 0.1], "scale": [1, 1, 1]}}}]})";
    CHECK(deserializeScene(old, legacyFile));
    CHECK(sameRotation(old.findEntity("Old").transform().rotation.toDouble(), glm::dquat(glm::dvec3(0.3, -1.2, 0.1))));

    // Version 1 angles: the field of view and the rotator in radians, hinge limits and motor in degrees.
    Scene v1;
    const char* v1File = R"({"format": "nova.scene", "version": 1, "entities": [
        {"components": {"Name": {"name": "Cam"}, "Camera": {"fovY": 1.0471975511965976}}},
        {"components": {"Name": {"name": "Spin"}, "Rotator": {"axis": [0, 1, 0], "speed": 3.141592653589793}}},
        {"components": {"Name": {"name": "Door"}, "Joint": {"type": "Hinge", "limitMin": -30, "limitMax": 60, "motorTarget": 90}}}]})";
    CHECK(deserializeScene(v1, v1File));
    CHECK(v1.findEntity("Cam").get<CameraComponent>().fovY == Angle::fromDegrees(60));
    CHECK(v1.findEntity("Spin").get<RotatorComponent>().speed == AngleSpeed::fromTurnsPerSecond(0.5));
    const JointComponent& door = v1.findEntity("Door").get<JointComponent>();
    CHECK(door.angleMin == Angle::fromDegrees(-30) && door.angleMax == Angle::fromDegrees(60));
    CHECK(door.motorSpeed == AngleSpeed::fromDegreesPerSecond(90) && door.motorAngle == Angle::fromDegrees(90));
    // Saved again, as version 2: degrees, bit for bit back.
    Scene v2;
    CHECK(deserializeScene(v2, serializeScene(v1)));
    CHECK(v2.findEntity("Cam").get<CameraComponent>().fovY == Angle::fromDegrees(60));
    CHECK(v2.findEntity("Spin").get<RotatorComponent>().speed == v1.findEntity("Spin").get<RotatorComponent>().speed);
    CHECK(v2.findEntity("Door").get<JointComponent>().angleMax == Angle::fromDegrees(60));
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Log::init();
    Log::core().set_level(spdlog::level::off); // the tests trigger expected warnings
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"glm: quantized in, floats out, compact rotations", glmInterop},
        {"axis-angle: quarter turns, wraps, composition", axisAngle},
        {"unit length: long chains, renormalization, no overflow", unitLength},
        {"Euler angles in six orders, gimbal lock, transforms", euler},
        {"slerp / nlerp / angleBetween / rotateTowards", interpolation},
        {"scene files: exact quaternions, legacy Euler", sceneFiles},
    };
    for (auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
