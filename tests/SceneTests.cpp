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
    CHECK(reflect::types().find("Transform")->field("rotation")->attrs.degrees);

    // Script reflection.
    const reflect::TypeInfo* patrol = reflect::types().find<Patrol>();
    CHECK(patrol && patrol->isScript && patrol->createScript() != nullptr);
}

// ---------------------------------------------------------- serialization --
void roundTrip() {
    Scene a("Arena");
    Entity e = a.createEntity("Hero");
    e.transform().position = {1, 2, 3};
    e.transform().rotation = {0.1f, 0.2f, 0.3f};
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
    CHECK(std::abs(h.transform().rotation.z - 0.3f) < 1e-6f);
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

void uuids() {
    Scene s;
    Entity a = s.createEntity("A"), b = s.createEntity("B");
    CHECK(a.uuid() != 0 && b.uuid() != 0 && a.uuid() != b.uuid());
    CHECK(s.findEntityByUuid(b.uuid()) == b);
    CHECK(!s.findEntityByUuid(0).valid() && !s.findEntityByUuid(12345).valid());
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
        {"uuids", uuids},
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
