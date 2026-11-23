// Headless tests of reflection and scene serialization (engine only: no window, no GPU).
#include <nova/Nova.h>

#include <cmath>
#include <cstdio>
#include <functional>
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

// ------------------------------------------------------------ game types --
enum class Team { Red = 1, Blue = 7 };

struct Stats {
    float speed = 1.0f;
    glm::vec2 range{0.0f, 10.0f};
};

struct Player {
    std::string nick = "anon";
    Team team = Team::Red;
    i32 lives = 3;
    u32 flags = 0;
    bool alive = true;
    glm::vec4 tint{1.0f};
    Stats stats;
    float runtimeOnly = 0.0f; // transient
};

struct Patrol : Script {
    float radius = 2.0f;
    std::string route = "A";
};

void reflectTestTypes() {
    reflect::enumeration<Team>("Team").value("Red", Team::Red).value("Blue", Team::Blue);
    reflect::type<Stats>("Stats").field("speed", &Stats::speed).field("range", &Stats::range);
    reflect::type<Player>("Player")
        .field("nick", &Player::nick)
        .field("team", &Player::team)
        .field("lives", &Player::lives, {.min = 0, .max = 9})
        .field("flags", &Player::flags)
        .field("alive", &Player::alive)
        .field("tint", &Player::tint, {.color = true})
        .field("stats", &Player::stats)
        .field("runtimeOnly", &Player::runtimeOnly, {.transient = true})
        .component();
    reflect::type<Patrol>("Patrol").field("radius", &Patrol::radius).field("route", &Patrol::route).script();
}

// ------------------------------------------------------------- reflection --
void reflection() {
    CHECK(reflect::prettifyName("linearDamping") == "Linear Damping");
    CHECK(reflect::prettifyName("RigidBody") == "Rigid Body");
    CHECK(reflect::prettifyName("max_hp") == "Max Hp");
    CHECK(reflect::prettifyName("v2Speed") == "V2 Speed");

    const reflect::TypeInfo* player = reflect::types().find<Player>();
    CHECK(player && player->isComponent && player->displayName == "Player");
    CHECK(reflect::types().find("Player") == player);
    using K = reflect::FieldKind;
    CHECK(player->field("nick")->kind == K::String);
    CHECK(player->field("team")->kind == K::Enum);
    CHECK(player->field("lives")->kind == K::Int && player->field("lives")->attrs.max == 9.0f);
    CHECK(player->field("flags")->kind == K::UInt);
    CHECK(player->field("tint")->kind == K::Vec4 && player->field("tint")->attrs.color);
    CHECK(player->field("stats")->kind == K::Struct && player->field("stats")->nestedType() == reflect::types().find<Stats>());
    CHECK(player->field("runtimeOnly")->label == "Runtime Only");
    CHECK(!player->field("missing"));

    const reflect::TypeInfo* team = reflect::types().find<Team>();
    CHECK(team && team->isEnum && std::string(team->enumName(7)) == "Blue" && team->enumValue("Red") == 1);
    CHECK(!team->enumName(3) && !team->enumValue("Green"));

    // Generic access through FieldInfo.
    Player p;
    player->field("lives")->as<i32>(&p) = 5;
    CHECK(p.lives == 5);
    player->field("team")->setEnum(player->field("team")->in(&p), 7);
    CHECK(p.team == Team::Blue);

    // Engine types are registered on first access.
    const reflect::TypeInfo* rb = reflect::types().find("RigidBody");
    CHECK(rb && rb->isComponent && rb->field("body")->attrs.transient);
    CHECK(reflect::types().find("Transform")->field("rotation")->kind == reflect::FieldKind::FixedQuat); // edited as Euler degrees

    // Script reflection.
    const reflect::TypeInfo* patrol = reflect::types().find<Patrol>();
    CHECK(patrol && patrol->isScript && patrol->createScript() != nullptr);
}

// ---------------------------------------------------------- serialization --
void roundTrip() {
    Scene a("Arena");
    Entity e = a.createEntity("Hero");
    e.transform().position = {1, 2, 3};
    e.transform().setEuler({0.1f, 0.2f, 0.3f});
    auto& mesh = e.add<MeshComponent>(MeshHandle{4});
    mesh.material.baseColor = {0.1f, 0.2f, 0.3f, 0.4f};
    mesh.material.metallic = 0.7f;
    mesh.castShadows = false;
    auto& rb = e.add<RigidBodyComponent>(physics::BodyType::Kinematic);
    rb.mass = 12.5f;
    rb.body = physics::BodyId{99}; // transient: must not survive
    e.add<SphereColliderComponent>().radius = 0.75f;
    auto& pl = e.add<Player>();
    pl.nick = "neo";
    pl.team = Team::Blue;
    pl.lives = 7;
    pl.flags = 0xF00Du;
    pl.alive = false;
    pl.tint = {0.5f, 0.6f, 0.7f, 0.8f};
    pl.stats.speed = 4.5f;
    pl.stats.range = {2.0f, 30.0f};
    pl.runtimeOnly = 123.0f;
    auto& patrol = e.addScript<Patrol>();
    patrol.radius = 9.0f;
    patrol.route = "B-C";
    Entity cam = a.createEntity("Cam");
    cam.add<CameraComponent>().fovY = glm::radians(75.0f);

    const std::string json = serializeScene(a);
    CHECK(json.find("\"format\": \"nova.scene\"") != std::string::npos);
    CHECK(json.find("\"team\": \"Blue\"") != std::string::npos); // enums by name
    CHECK(json.find("runtimeOnly") == std::string::npos);       // transient skipped
    CHECK(json.find("\"body\"") == std::string::npos);

    Scene b;
    std::string error;
    CHECK(deserializeScene(b, json, {}, &error));
    CHECK(b.name() == "Arena");
    Entity h = b.findEntityByUuid(e.uuid());
    CHECK(h.valid() && h.name() == "Hero");
    if (!h.valid()) return;
    CHECK(h.transform().position == glm::vec3(1, 2, 3));
    CHECK(h.transform().rotation == e.transform().rotation); // Q1.30 quaternion stored bit for bit
    CHECK(h.get<MeshComponent>().mesh.id == 4 && !h.get<MeshComponent>().castShadows);
    CHECK(h.get<MeshComponent>().material.baseColor == glm::vec4(0.1f, 0.2f, 0.3f, 0.4f));
    CHECK(h.get<MeshComponent>().material.metallic == 0.7f);
    CHECK(h.get<RigidBodyComponent>().type == physics::BodyType::Kinematic && h.get<RigidBodyComponent>().mass == 12.5f);
    CHECK(!h.get<RigidBodyComponent>().body.valid());
    CHECK(h.get<SphereColliderComponent>().radius == 0.75f);
    const Player& q = h.get<Player>();
    CHECK(q.nick == "neo" && q.team == Team::Blue && q.lives == 7 && q.flags == 0xF00Du && !q.alive);
    CHECK(q.tint == glm::vec4(0.5f, 0.6f, 0.7f, 0.8f) && q.stats.speed == 4.5f && q.stats.range == glm::vec2(2, 30));
    CHECK(q.runtimeOnly == 0.0f);
    auto* sc = h.tryGet<ScriptComponent>();
    CHECK(sc && sc->scripts.size() == 1);
    if (sc && !sc->scripts.empty()) {
        auto& s = static_cast<Patrol&>(*sc->scripts[0]);
        CHECK(s.radius == 9.0f && s.route == "B-C" && s.entity() == h); // attached to the right entity
    }
    Entity c = b.findEntity("Cam");
    CHECK(c.valid() && std::abs(c.get<CameraComponent>().fovY - glm::radians(75.0f)) < 1e-5f);

    // Deterministic: serializing the loaded scene gives the same text.
    CHECK(serializeScene(b) == json);
}

void meshNames() {
    Scene a;
    a.createEntity("Box").add<MeshComponent>(MeshHandle{2});
    SerializeOptions byName;
    byName.meshName = [](MeshHandle m) { return m.id == 2 ? std::string("Cube") : std::string("?"); };
    byName.findMesh = [](const std::string& n) { return n == "Cube" ? MeshHandle{5} : MeshHandle{}; };
    const std::string json = serializeScene(a, byName);
    CHECK(json.find("\"mesh\": \"Cube\"") != std::string::npos);
    Scene b;
    CHECK(deserializeScene(b, json, byName));
    CHECK(b.findEntity("Box").get<MeshComponent>().mesh.id == 5); // resolved by name in the new session
}

void forwardCompatibility() {
    Log::core().set_level(spdlog::level::off); // the warnings below are expected
    const char* json = R"({
        "format": "nova.scene", "version": 1, "name": "Old",
        "entities": [
          { "components": {
              "Name": {"name": "Legacy"},
              "Player": {"nick": "x", "removedField": 5, "team": "Purple"},
              "DeletedComponent": {"a": 1} },
            "scripts": [ {"type": "GoneScript", "fields": {}}, {"type": "Patrol"} ] }
        ] })";
    Scene s;
    std::string error;
    CHECK(deserializeScene(s, json, {}, &error));
    Entity e = s.findEntity("Legacy");
    CHECK(e.valid() && e.has<Player>());
    if (e.valid()) {
        CHECK(e.get<Player>().nick == "x");
        CHECK(e.get<Player>().lives == 3);          // missing field keeps its default
        CHECK(e.get<Player>().team == Team::Red);   // unknown enum name keeps the default
        CHECK(e.uuid() != 0);                       // no Id in file: a fresh one
        auto* sc = e.tryGet<ScriptComponent>();
        CHECK(sc && sc->scripts.size() == 1);       // unknown script skipped, known one loaded
        if (sc && !sc->scripts.empty()) CHECK(static_cast<Patrol&>(*sc->scripts[0]).radius == 2.0f);
    }
    Log::core().set_level(spdlog::level::warn);

    CHECK(!deserializeScene(s, "{ not json", {}, &error) && error == "invalid JSON");
    CHECK(!deserializeScene(s, R"({"format": "other"})", {}, &error));
    CHECK(!deserializeScene(s, R"({"format": "nova.scene", "version": 99})", {}, &error));
}

void files() {
    Scene a("Ignored");
    a.createEntity("OnDisk").transform().scale = {2, 2, 2};
    const auto path = std::filesystem::temp_directory_path() / "nova_scene_test.scene.json";
    CHECK(saveScene(a, path));
    Scene b;
    CHECK(loadScene(b, path));
    CHECK(b.name() == "nova_scene_test");
    CHECK(b.findEntity("OnDisk").valid() && b.findEntity("OnDisk").transform().scale == glm::vec3(2));
    std::filesystem::remove(path);
    std::string error;
    CHECK(!loadScene(b, path, {}, &error) && !error.empty());
    CHECK(sceneNameFromPath("levels/forest.scene.json") == "forest");
    CHECK(sceneNameFromPath("plain.json") == "plain");
}

void duplication() {
    Scene s;
    Entity e = s.createEntity("Orc");
    e.add<Player>().lives = 1;
    e.addScript<Patrol>().radius = 4.0f;
    Entity copy = duplicateEntity(e);
    CHECK(copy.valid() && copy != e);
    CHECK(copy.name() == "Orc (copy)");
    CHECK(copy.uuid() != 0 && copy.uuid() != e.uuid());
    CHECK(copy.has<Player>() && copy.get<Player>().lives == 1);
    auto* sc = copy.tryGet<ScriptComponent>();
    CHECK(sc && sc->scripts.size() == 1 && static_cast<Patrol&>(*sc->scripts[0]).radius == 4.0f);
    CHECK(e.get<Player>().lives == 1 && e.name() == "Orc"); // original untouched
}

void floatsShortAndExact() {
    Scene a;
    Entity e = a.createEntity("F");
    const float values[] = {0.55f, 0.1f, 1.0f / 3.0f, 123456.789f, -2.5e-7f, 3.4e38f};
    e.add<PointLightComponent>();
    std::string json;
    for (float v : values) {
        e.get<PointLightComponent>().intensity = v;
        json = serializeScene(a);
        Scene b;
        CHECK(deserializeScene(b, json));
        CHECK(b.findEntity("F").get<PointLightComponent>().intensity == v); // bit-exact round trip
    }
    e.get<PointLightComponent>().intensity = 0.55f;
    json = serializeScene(a);
    CHECK(json.find("\"intensity\": 0.55") != std::string::npos);
    CHECK(json.find("0.5500000") == std::string::npos);
}

// Simulation attributes are fixed point: reflected with their format, written as the shortest
// decimal that reads back to the same bits, edited state survives bit for bit.
void fixedPointFields() {
    const reflect::TypeInfo* rb = reflect::types().find("RigidBody");
    CHECK(rb->field("mass")->kind == reflect::FieldKind::Fixed && rb->field("mass")->fracBits == 16);
    const reflect::TypeInfo* box = reflect::types().find("BoxCollider");
    CHECK(box->field("halfExtents")->kind == reflect::FieldKind::FixedVec3 && box->field("halfExtents")->fracBits == 18);
    CHECK(box->field("rotation")->kind == reflect::FieldKind::FixedQuat);
    CHECK(reflect::types().find("Transform")->field("scale")->kind == reflect::FieldKind::FixedVec3);
    CHECK(reflect::types().find("Joint")->field("breakForce")->fracBits == 8);
    CHECK(reflect::types().find("CharacterController")->field("velocity")->fracBits == 16);

    // Generic access through the reflection: exact values, nearest on write.
    RigidBodyComponent body;
    const reflect::FieldInfo& mass = *rb->field("mass");
    reflect::setFixedValue(mass, mass.in(&body), 12.3);
    CHECK(body.mass == Scalar(12.3) && reflect::fixedValue(mass, mass.in(&body)) == toDouble(body.mass));

    Scene a;
    Entity e = a.createEntity("Fx");
    auto& r = e.add<RigidBodyComponent>();
    r.friction = 0.05f;  // raw 3277: 0.0500030517578125
    r.mass = 1.0 / 3.0;
    auto& b = e.add<BoxColliderComponent>();
    b.halfExtents = {0.1, 2.75, 1e-5};
    b.rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
    e.transform().scale = {1.5, 0.2, 3};
    e.transform().rotate(0.4f, {0, 1, 0});
    PhysicsSettings ps;
    ps.gravity = {0.0, -1.62, 0.0};
    a.setPhysicsSettings(ps);
    const std::string json = serializeScene(a);
    CHECK(json.find("\"friction\": 0.05") != std::string::npos); // short, not 0.0500030517578125
    CHECK(json.find("0.050003") == std::string::npos);
    Scene c;
    CHECK(deserializeScene(c, json));
    Entity h = c.findEntity("Fx");
    CHECK(h.get<RigidBodyComponent>().friction == r.friction && h.get<RigidBodyComponent>().mass == r.mass);
    CHECK(h.get<BoxColliderComponent>().halfExtents == b.halfExtents);
    CHECK(h.get<BoxColliderComponent>().rotation == b.rotation);
    CHECK(h.transform().scale == e.transform().scale && h.transform().rotation == e.transform().rotation);
    CHECK(c.physicsSettings().gravity == ps.gravity);
    CHECK(serializeScene(c) == json);

    // Every raw value of a format survives a file (a sample across the whole range).
    u32 seed = 12345;
    for (int i = 0; i < 2000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        r.mass = Scalar::from_raw_value(static_cast<i32>(seed));
        b.offset.x = Length::from_raw_value(static_cast<i32>(seed * 2654435761u));
        Scene d;
        CHECK(deserializeScene(d, serializeScene(a)));
        CHECK(d.findEntity("Fx").get<RigidBodyComponent>().mass.raw_value() == r.mass.raw_value());
        CHECK(d.findEntity("Fx").get<BoxColliderComponent>().offset.x.raw_value() == b.offset.x.raw_value());
    }
}

// Scripts get the exact frame time (onUpdate(Duration)); onUpdate(float) stays as the shortcut.
void scriptTime() {
    struct Exact : Script {
        Duration total;
        void onUpdate(Duration dt) override { total += dt; }
    };
    struct Seconds : Script {
        f32 total = 0.0f;
        void onUpdate(f32 dt) override { total += dt; }
    };
    Scene s;
    Entity e = s.createEntity("S");
    auto& exact = e.addScript<Exact>();
    auto& seconds = e.addScript<Seconds>();
    for (int i = 0; i < 600; ++i) s.update(Duration::perSecond(60));
    CHECK(exact.total == Duration::fromSeconds(10)); // 600 × 1/60 s: exact in flicks
    CHECK(std::abs(seconds.total - 10.0f) < 1e-3f);
}

void meshRaycast() {
    const MeshData cube = MeshData::cube(); // unit cube centered at origin
    // Scaled ×2 and moved to x = 10: faces at x = 9 and 11.
    glm::mat4 m = glm::translate(glm::mat4(1.0f), {10, 0, 0}) * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    auto hit = raycastMesh(cube, m, {0, 0, 0}, {1, 0, 0});
    CHECK(hit && std::abs(hit->distance - 9.0f) < 1e-4f);
    if (hit) CHECK(glm::length(hit->normal - glm::vec3(-1, 0, 0)) < 1e-4f && std::abs(hit->point.x - 9.0f) < 1e-4f);
    CHECK(!raycastMesh(cube, m, {0, 0, 0}, {-1, 0, 0}));          // pointing away
    CHECK(!raycastMesh(cube, m, {0, 5, 0}, {1, 0, 0}));           // passes above
    CHECK(!raycastMesh(cube, m, {0, 0, 0}, {1, 0, 0}, 5.0f));     // too short

    // Rotated 45° around Y: the nearest edge is at x = 10 - sqrt(2).
    glm::mat4 r = glm::translate(glm::mat4(1.0f), {10, 0, 0}) * glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), {0, 1, 0}) *
                  glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    hit = raycastMesh(cube, r, {0, 0.5f, 0.01f}, {1, 0, 0});
    CHECK(hit && std::abs(hit->distance - (10.0f - std::sqrt(2.0f))) < 0.02f);

    const MeshData sphere = MeshData::sphere(0.5f, 64, 32);
    hit = raycastMesh(sphere, glm::mat4(1.0f), {0, 5, 0}, {0, -1, 0});
    // Straight through the pole vertex (shared by many triangles; the opposite pole has degenerate ones).
    CHECK(hit && std::abs(hit->distance - 4.5f) < 0.01f && hit->normal.y > 0.99f);
    hit = raycastMesh(sphere, glm::mat4(1.0f), {0, -5, 0}, {0, 1, 0});
    CHECK(hit && std::abs(hit->distance - 4.5f) < 0.01f && hit->normal.y < -0.99f);

    Bounds b = cube.bounds();
    CHECK(b.min == glm::vec3(-0.5f) && b.max == glm::vec3(0.5f) && b.extents() == glm::vec3(0.5f));
}

void debugDraw() {
    DebugDraw dd;
    dd.line({0, 0, 0}, {1, 0, 0}, {1, 0, 0, 1});
    CHECK(dd.lineCount() == 1 && dd.vertices(false).size() == 2 && dd.vertices(true).empty());
    dd.box({0, 0, 0}, glm::vec3(1), glm::quat(1, 0, 0, 0), {1, 1, 1, 1}, true);
    CHECK(dd.vertices(true).size() == 24); // 12 edges on top
    dd.sphere({0, 0, 0}, 1.0f, {1, 1, 1, 1});
    CHECK(dd.lineCount() == 1 + 12 + 96);
    dd.arrow({0, 0, 0}, {0, 2, 0}, {1, 1, 1, 1});
    CHECK(dd.lineCount() == 1 + 12 + 96 + 5);
    CHECK(DebugDraw::pack({1, 0, 0, 1}) == 0xFF0000FFu); // RGBA8 little-endian, as unpackUnorm4x8 expects
    // Box corners follow the rotation.
    DebugDraw r;
    r.box({0, 0, 0}, {1, 0.1f, 0.1f}, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)), {1, 1, 1, 1});
    f32 maxY = 0;
    for (const auto& v : r.vertices(false)) maxY = std::max(maxY, v.position.y);
    CHECK(std::abs(maxY - 1.0f) < 1e-4f); // the long axis now points up
    dd.clear();
    CHECK(dd.lineCount() == 0);
}

void entityRestoreInPlace() {
    Scene s;
    Entity e = s.createEntity("Thing");
    e.transform().position = {1, 1, 1};
    e.add<PointLightComponent>().intensity = 5.0f;
    const std::string before = serializeEntity(e);

    e.transform().position = {9, 9, 9};
    e.remove<PointLightComponent>();
    e.add<Player>();
    e.get<NameComponent>().name = "Renamed";

    Entity restored = restoreEntity(s, before);
    CHECK(restored == e); // same handle: restored in place
    CHECK(e.name() == "Thing" && e.transform().position == glm::vec3(1, 1, 1));
    CHECK(e.has<PointLightComponent>() && e.get<PointLightComponent>().intensity == 5.0f);
    CHECK(!e.has<Player>()); // components added since the snapshot are removed

    const u64 id = e.uuid();
    s.destroyEntity(e);
    Entity back = restoreEntity(s, before);
    CHECK(back.valid() && back.uuid() == id && back.name() == "Thing"); // recreated with the same UUID
    CHECK(!restoreEntity(s, "{ broken").valid());
}

void uuids() {
    Scene s;
    Entity a = s.createEntity("A"), b = s.createEntity("B");
    CHECK(a.uuid() != 0 && b.uuid() != 0 && a.uuid() != b.uuid());
    CHECK(s.findEntityByUuid(b.uuid()) == b);
    CHECK(!s.findEntityByUuid(0).valid() && !s.findEntityByUuid(12345).valid());
}

bool near(const GlobalPosition& p, glm::dvec3 m, f64 eps = 1e-4) { return glm::length((p - GlobalPosition(m)).toMeters()) < eps; }

void hierarchy() {
    Scene scene{"tree"};
    Entity car = scene.createEntity("Car");
    car.transform().position = {10, 0, 0};
    Entity wheel = scene.createEntity("Wheel");
    wheel.transform().position = {12, 0, 1};
    Entity bolt = scene.createEntity("Bolt");
    bolt.transform().position = {12, 0, 2};

    // Parenting keeps the pose in the world.
    CHECK(wheel.setParent(car) && bolt.setParent(wheel));
    CHECK(near(wheel.transform().position, {12, 0, 1}) && wheel.parent() == car && bolt.parent() == wheel);
    CHECK(car.children().size() == 1 && car.children()[0] == wheel && !car.parent().valid());
    CHECK(scene.descendantsOf(car).size() == 2 && scene.descendantsOf(car)[1] == bolt);
    // No loops: a parent cannot become the child of its descendant (nor of itself).
    CHECK(!car.setParent(bolt) && !car.setParent(car) && !car.parent().valid());

    // The parent moves, turns and scales: the children follow, keeping their offsets.
    car.transform().position = {20, 0, 0};
    scene.updateTransforms();
    CHECK(near(wheel.transform().position, {22, 0, 1}) && near(bolt.transform().position, {22, 0, 2}));
    car.transform().setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0))); // +X turns to −Z
    scene.updateTransforms();
    CHECK(near(wheel.transform().position, {21, 0, -2}, 1e-3));
    CHECK(std::abs(glm::dot(wheel.transform().orientation(), car.transform().orientation())) > 0.9999f);
    car.transform().scale = glm::vec3(2.0f);
    scene.updateTransforms();
    CHECK(near(wheel.transform().position, {22, 0, -4}, 1e-3) && std::abs(wheel.transform().scaleF().x - 2.0f) < 1e-4f);
    CHECK(near(bolt.transform().position, {24, 0, -4}, 1e-3)); // its offset from the wheel, scaled too

    // Offsets: read and set in the parent's frame (before its scale).
    const LocalTransform local = scene.localTransform(wheel);
    CHECK(glm::length(local.position - glm::dvec3(2, 0, 1)) < 1e-6 && glm::length(local.scale - glm::dvec3(1.0)) < 1e-6);
    scene.setLocalTransform(wheel, {glm::dvec3(0, 1, 0), glm::dquat(1, 0, 0, 0), glm::dvec3(1.0)});
    CHECK(near(wheel.transform().position, {20, 2, 0}, 1e-3));

    // Moved on its own (a script, the gizmo): it keeps the new pose, and its offset from there.
    car.transform().scale = glm::vec3(1.0f);
    car.transform().setRotation(glm::quat(1, 0, 0, 0));
    scene.updateTransforms();
    wheel.transform().position = {25, 0, 0};
    scene.updateTransforms();
    CHECK(near(wheel.transform().position, {25, 0, 0}));
    car.transform().position = {30, 0, 0};
    scene.updateTransforms();
    CHECK(near(wheel.transform().position, {35, 0, 0}));

    // A still hierarchy stays bit for bit where it is (nothing is recomputed).
    const GlobalPosition still = bolt.transform().position;
    for (int i = 0; i < 5; ++i) scene.updateTransforms();
    CHECK(bolt.transform().position == still);

    // update() keeps them in step after scripts and built-in systems: a turning parent swings them.
    Entity hub = scene.createEntity("Hub");
    hub.add<RotatorComponent>(glm::vec3(0, 1, 0), glm::half_pi<f32>()); // 90°/s
    Entity arm = scene.createEntity("Arm");
    arm.transform().position = {1, 0, 0};
    arm.setParent(hub);
    scene.update(Duration::fromSeconds(1.0));
    CHECK(near(arm.transform().position, {0, 0, -1}, 1e-3));

    // Across the seam of a small world: the offset goes the short way.
    {
        Scene small;
        small.setWorldSize(WorldSize::cube(20.0));
        Entity p = small.createEntity("P");
        p.transform().position = {9, 0, 0};
        Entity c = small.createEntity("C");
        c.transform().position = {-9, 0, 0}; // 2 m past the seam from P
        c.setParent(p);
        CHECK(glm::length(small.localTransform(c).position - glm::dvec3(2, 0, 0)) < 1e-6);
        p.transform().position = {8, 0, 0};
        small.updateTransforms();
        CHECK(c.transform().position == GlobalPosition(10, 0, 0)); // ≡ −10
        WorldSize::setActive({});
    }

    // Saved: only the reference; after loading the children follow again.
    const std::string json = serializeScene(scene);
    Scene loaded;
    CHECK(deserializeScene(loaded, json));
    Entity car2 = loaded.findEntity("Car"), wheel2 = loaded.findEntity("Wheel");
    CHECK(wheel2.parent() == car2 && wheel2.transform().position == wheel.transform().position);
    const glm::dvec3 boltBefore = loaded.findEntity("Bolt").transform().position.toMeters();
    car2.transform().position = {40, 0, 0};
    loaded.updateTransforms();
    CHECK(near(wheel2.transform().position, {45, 0, 0}));
    CHECK(near(loaded.findEntity("Bolt").transform().position, boltBefore + glm::dvec3(10, 0, 0), 1e-3)); // the grandchild too

    // Duplicating copies the whole subtree; the copied children follow the copy.
    const size_t before = scene.registry().view<IdComponent>().size();
    Entity carCopy = duplicateEntity(car);
    CHECK(scene.registry().view<IdComponent>().size() == before + 3);
    CHECK(carCopy.children().size() == 1 && carCopy.children()[0] != wheel && carCopy.children()[0].children().size() == 1);
    CHECK(car.children().size() == 1 && car.children()[0] == wheel); // the original is untouched
    Entity wheelCopy = duplicateEntity(wheel); // a child's copy is a sibling: same parent
    CHECK(wheelCopy.parent() == car && wheelCopy.children().size() == 1);

    // A prefab takes the descendants along; a parent left out is forgotten.
    const std::string prefab = serializePrefab({car});
    Scene other;
    auto made = instantiatePrefab(other, prefab, GlobalPosition(100, 0, 0));
    CHECK(made.size() == 3 + 2); // car, wheel, bolt, and the copy of the wheel with its bolt
    Entity root = made[0];
    CHECK(!root.parent().valid() && root.children().size() == 2);
    root.transform().position = {110, 0, 0};
    other.updateTransforms();
    CHECK(near(other.findEntity("Wheel").transform().position, {115, 0, 0}));
    auto lone = instantiatePrefab(other, serializePrefab({wheel}), GlobalPosition(0, 0, 0));
    CHECK(lone.size() == 2 && !lone[0].parent().valid() && lone[1].parent() == lone[0]);

    // Destroying a parent takes its descendants along.
    scene.destroyEntity(car);
    CHECK(!car.valid() && !wheel.valid() && !bolt.valid() && !wheelCopy.valid() && carCopy.valid());

    // The inspector sets the reference directly: the entity keeps its pose in the world.
    Entity a = scene.createEntity("A"), b = scene.createEntity("B");
    a.transform().position = {1, 2, 3};
    b.transform().position = {5, 5, 5};
    b.add<ParentComponent>().parent = Scene::refOf(a);
    scene.updateTransforms();
    CHECK(near(b.transform().position, {5, 5, 5}) && b.parent() == a);
    a.transform().position = {2, 2, 3};
    scene.updateTransforms();
    CHECK(near(b.transform().position, {6, 5, 5}));
}

void fullMaterials() {
    // The full material is saved with the scene: maps' strengths, alpha mode and cutoff, both sides.
    Scene scene;
    Entity e = scene.createEntity("Leaf");
    Material& m = e.add<MeshComponent>(MeshHandle{1}).material;
    m.normalScale = 2.0f;
    m.occlusionStrength = 0.5f;
    m.alphaMode = AlphaMode::Mask;
    m.alphaCutoff = 0.3f;
    m.doubleSided = true;
    m.uvScale = {4.0f, 2.0f};
    m.uvRotation = 0.5f;
    m.uvOffset = {0.25f, 0.0f};
    Entity glass = scene.createEntity("Glass");
    glass.add<MeshComponent>(MeshHandle{1}).material.alphaMode = AlphaMode::Blend;
    const std::string json = serializeScene(scene);
    CHECK(json.find("\"alphaMode\": \"Mask\"") != std::string::npos && json.find("\"doubleSided\": true") != std::string::npos);
    Scene back;
    CHECK(deserializeScene(back, json));
    const Material& r = back.findEntity("Leaf").get<MeshComponent>().material;
    CHECK(r.normalScale == 2.0f && r.occlusionStrength == 0.5f && r.alphaMode == AlphaMode::Mask && r.alphaCutoff == 0.3f && r.doubleSided);
    CHECK(r.uvScale == glm::vec2(4.0f, 2.0f) && r.uvRotation == 0.5f && r.uvOffset == glm::vec2(0.25f, 0.0f));
    CHECK(back.findEntity("Glass").get<MeshComponent>().material.alphaMode == AlphaMode::Blend);
    const Material plain;
    CHECK(plain.alphaMode == AlphaMode::Opaque && !plain.doubleSided && !plain.normalTexture && plain.normalScale == 1.0f);
}

} // namespace

int main() {
    Log::init();
    Log::core().set_level(spdlog::level::warn);
    reflectTestTypes();
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"reflection", reflection},
        {"round trip (all field kinds, scripts, transient)", roundTrip},
        {"meshes by name", meshNames},
        {"forward compatibility and errors", forwardCompatibility},
        {"files", files},
        {"duplication", duplication},
        {"floats are short and exact", floatsShortAndExact},
        {"fixed-point fields: reflection, short decimals, exact round trips", fixedPointFields},
        {"scripts: exact frame time", scriptTime},
        {"mesh raycast", meshRaycast},
        {"debug draw", debugDraw},
        {"entity restore in place", entityRestoreInPlace},
        {"uuids", uuids},
        {"full materials are saved (maps, alpha, double sided)", fullMaterials},
        {"hierarchy: follow, offsets, loops, saving, duplicate, prefabs, destroy", hierarchy},
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
