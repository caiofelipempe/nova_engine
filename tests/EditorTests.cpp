// Headless tests of the editor framework's logic (no window, no GPU, no ImGui frame).
#include <nova/editor/Editor.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace nova;
using namespace nova::editor;

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

// Context + registry over a scene, as the editor wires them.
struct Fixture {
    Scene scene{"test"};
    EditorContext ctx{scene};
    Registry registry{ctx};
    Fixture() { ctx.setRegistry(&registry); }
};

struct CountingPanel : Panel {
    int* attached;
    int* detached;
    CountingPanel(std::string title, int* a, int* d) : Panel(std::move(title)), attached(a), detached(d) {}
    void onAttach(EditorContext&) override { ++*attached; }
    void onDetach(EditorContext&) override { ++*detached; }
    void onImGui(EditorContext&) override {}
};

struct Health {
    float current = 100.0f;
};

// ------------------------------------------------------------------ chords --
void keyChordParsing() {
    auto c = KeyChord::parse("Ctrl+Shift+S");
    CHECK(c && c->key == Key::S && c->ctrl && c->shift && !c->alt);
    CHECK(c && c->toString() == "Ctrl+Shift+S");

    CHECK((KeyChord::parse(" ctrl + p ") == KeyChord{Key::P, true, false, false})); // case + spaces
    CHECK(KeyChord::parse("Delete")->key == Key::Delete);
    CHECK(KeyChord::parse("del")->key == Key::Delete);
    CHECK(KeyChord::parse("F5")->key == Key::F5);
    CHECK(KeyChord::parse("Shift+F12")->toString() == "Shift+F12");
    CHECK(KeyChord::parse("Alt+7")->key == Key::D7);
    CHECK(KeyChord::parse("Escape")->toString() == "Escape");
    CHECK(KeyChord::parse("Esc")->toString() == "Escape"); // alias -> canonical name

    CHECK(!KeyChord::parse(""));
    CHECK(!KeyChord::parse("Ctrl"));        // modifiers only
    CHECK(!KeyChord::parse("Ctrl+Foo"));    // unknown key
    CHECK(!KeyChord::parse("A+B"));         // two main keys
    CHECK(!KeyChord::parse("F13"));         // out of range
    CHECK(!KeyChord::parse("Ctrl++S"));     // empty token
    CHECK(KeyChord{}.toString().empty());
}

void menuPathSplitting() {
    auto parts = splitMenuPath("File//Recent/scene.json/");
    CHECK(parts.size() == 3 && parts[0] == "File" && parts[1] == "Recent" && parts[2] == "scene.json");
    CHECK(splitMenuPath("").empty());
}

// --------------------------------------------------------------- commands --
void commands() {
    Fixture f;
    int runs = 0;
    Command& c = f.registry.command("Tools/Bake", [&](EditorContext&) { ++runs; }, "Ctrl+B");
    CHECK(c.id == "Tools/Bake" && c.menuPath == "Tools/Bake");
    CHECK(c.shortcut == (KeyChord{Key::B, true, false, false}));
    CHECK(f.registry.execute("Tools/Bake") && runs == 1);

    // Same id replaces (no duplicates); references to stored commands stay valid while others are added.
    f.registry.command("Tools/Bake", [&](EditorContext&) { runs += 10; });
    CHECK(f.registry.commands().size() == 1);
    for (int i = 0; i < 100; ++i) f.registry.command("Tools/Other " + std::to_string(i), [](EditorContext&) {});
    CHECK(c.id == "Tools/Bake"); // still the same element
    f.registry.execute("Tools/Bake");
    CHECK(runs == 11);

    bool enabled = false;
    Command gated;
    gated.id = "gated";
    gated.execute = [&](EditorContext&) { ++runs; };
    gated.isEnabled = [&](EditorContext&) { return enabled; };
    f.registry.command(std::move(gated));
    CHECK(!f.registry.execute("gated"));
    enabled = true;
    CHECK(f.registry.execute("gated") && runs == 12);

    CHECK(!f.registry.execute("missing"));
    CHECK(f.registry.removeCommand("gated") && !f.registry.findCommand("gated"));

    // Invalid shortcut text: command still registered, without a shortcut.
    Log::core().set_level(spdlog::level::off);
    Command& bad = f.registry.command("Tools/Bad", [](EditorContext&) {}, "Ctrl+Nope");
    Log::core().set_level(spdlog::level::warn);
    CHECK(!bad.shortcut.valid());
}

// ----------------------------------------------------------------- panels --
void panels() {
    int attached = 0, detached = 0;
    {
        Fixture f;
        f.registry.panel<CountingPanel>("A", &attached, &detached);
        CHECK(attached == 0); // not attached before the editor starts
        f.registry.attachAll();
        CHECK(attached == 1);

        f.registry.panel<CountingPanel>("B", &attached, &detached); // registered later: attached immediately
        CHECK(attached == 2);

        f.registry.panel<CountingPanel>("A", &attached, &detached); // same title replaces
        CHECK(detached == 1 && attached == 3 && f.registry.panels().size() == 2);

        CHECK(f.registry.findPanel("B") != nullptr);
        CHECK(f.registry.findPanel<CountingPanel>() != nullptr);
        CHECK(f.registry.removePanel("B") && detached == 2 && !f.registry.findPanel("B"));
        CHECK(!f.registry.removePanel("B"));
    }
    CHECK(detached == 3); // remaining panel detached when the registry goes away
}

// ------------------------------------------------------- component editors --
void componentEditors() {
    Fixture f;
    float drawnWith = 0.0f;
    bool sawEntity = false, sawContext = false;
    f.registry.component<Health>("Health", [&](Health& h) { drawnWith = h.current; });
    f.registry.component<TransformComponent>("Transform", [&](TransformComponent&, Entity e) { sawEntity = e.valid(); });
    f.registry.component<NameComponent>("Name", [&](NameComponent&, Entity, EditorContext& c) { sawContext = &c == &f.ctx; });

    Entity e = f.scene.createEntity("E");
    const ComponentEditor* health = nullptr;
    for (auto& ed : f.registry.componentEditors())
        if (ed.name == "Health") health = &ed;
    CHECK(health && !health->has(e));
    health->add(e);
    CHECK(health->has(e));
    e.get<Health>().current = 42.0f;
    health->draw(e, f.ctx);
    CHECK(drawnWith == 42.0f);
    health->remove(e);
    CHECK(!e.has<Health>());

    for (auto& ed : f.registry.componentEditors())
        if (ed.has(e)) ed.draw(e, f.ctx);
    CHECK(sawEntity && sawContext); // all three draw signatures dispatched

    // Same name replaces; removal by name.
    f.registry.component<Health>("Health", [](Health&) {});
    CHECK(f.registry.componentEditors().size() == 3);
    CHECK(f.registry.removeComponentEditor("Health") && f.registry.componentEditors().size() == 2);
}

struct TestPlugin : Plugin {
    void onRegister(Registry& r) override {
        r.command("Test/Hello", [](EditorContext&) {});
        r.component<Health>("Health", [](Health&) {});
    }
};

void plugins() {
    Fixture f;
    f.registry.plugin<TestPlugin>();
    CHECK(f.registry.findCommand("Test/Hello") != nullptr);
    CHECK(f.registry.componentEditors().size() == 1);
}

// ---------------------------------------------------------------- context --
void selection() {
    Fixture f;
    int changes = 0;
    Entity last;
    f.ctx.selectionChanged.connect([&](Entity e) {
        ++changes;
        last = e;
    });
    Entity a = f.scene.createEntity("A");
    f.ctx.select(a);
    f.ctx.select(a); // same: no signal
    CHECK(changes == 1 && last == a && f.ctx.selected() == a);

    f.scene.destroyEntity(a);
    f.ctx.validateSelection();
    CHECK(changes == 2 && !f.ctx.selected().valid());
    f.ctx.select(a); // destroyed entity = clear (already clear: no signal)
    CHECK(changes == 2);
}

void playPauseStop() {
    Fixture f;
    Entity ground = f.scene.createEntity("Ground");
    ground.transform().position = {0, -0.5f, 0};
    ground.transform().scale = {50, 1, 50};
    ground.add<RigidBodyComponent>(physics::BodyType::Static);
    Entity box = f.scene.createEntity("Box");
    box.transform().position = {0, 5, 0};
    box.add<RigidBodyComponent>();

    std::vector<PlayState> states;
    f.ctx.playStateChanged.connect([&](PlayState s) { states.push_back(s); });

    f.ctx.play();
    CHECK(f.ctx.playState() == PlayState::Play && f.scene.isSimulating());
    for (int i = 0; i < 30; ++i) f.scene.update(1.0f / 60.0f);
    const float fallen = box.transform().position.y;
    CHECK(fallen < 4.9f);

    f.ctx.pause();
    f.ctx.play(); // resume: must not take a new snapshot
    for (int i = 0; i < 30; ++i) f.scene.update(1.0f / 60.0f);
    CHECK(box.transform().position.y < fallen);

    const u64 boxId = box.uuid();
    f.ctx.select(box);
    f.ctx.stop();
    CHECK(f.ctx.playState() == PlayState::Edit && !f.scene.isSimulating());
    // Stop rebuilds the scene from the snapshot: handles change, UUIDs don't.
    Entity restored = f.scene.findEntityByUuid(boxId);
    CHECK(restored.valid());
    if (restored.valid()) CHECK(std::abs(restored.transform().position.y - 5.0f) < 1e-4f);
    CHECK(f.ctx.selected() == restored); // selection follows the UUID
    CHECK((states == std::vector<PlayState>{PlayState::Play, PlayState::Pause, PlayState::Play, PlayState::Edit}));

    f.ctx.stop(); // no-op in Edit
    f.ctx.pause();
    CHECK(states.size() == 4);
}

struct Spinner : Script {
    float speed = 1.0f;
    void onUpdate(float dt) override { transform().rotation.y += speed * dt; }
};

void stopRestoresWholeScene() {
    Fixture f;
    reflect::type<Spinner>("TestSpinner").field("speed", &Spinner::speed).script();
    Entity keep = f.scene.createEntity("Keep");
    keep.addScript<Spinner>().speed = 3.0f;
    Entity doomed = f.scene.createEntity("Doomed");
    doomed.add<PointLightComponent>().intensity = 42.0f;
    const u64 keepId = keep.uuid(), doomedId = doomed.uuid();
    int reloads = 0;
    f.ctx.sceneReloaded.connect([&] { ++reloads; });

    f.ctx.play();
    for (int i = 0; i < 10; ++i) f.scene.update(0.1f);
    CHECK(keep.transform().rotation.y > 2.0f); // the script ran
    f.scene.destroyEntity(doomed);             // gameplay destroys an entity
    f.scene.createEntity("Spawned");            // ... and spawns another
    f.ctx.stop();

    CHECK(reloads == 1);
    CHECK(!f.scene.findEntity("Spawned").valid());
    Entity back = f.scene.findEntityByUuid(doomedId);
    CHECK(back.valid() && back.has<PointLightComponent>() && back.get<PointLightComponent>().intensity == 42.0f);
    Entity k = f.scene.findEntityByUuid(keepId);
    CHECK(k.valid() && k.transform().rotation.y == 0.0f); // script effect undone
    auto* sc = k.valid() ? k.tryGet<ScriptComponent>() : nullptr;
    CHECK(sc && sc->scripts.size() == 1 && static_cast<Spinner&>(*sc->scripts[0]).speed == 3.0f); // script + field restored
}

void sceneFiles() {
    Fixture f;
    Entity e = f.scene.createEntity("Saved");
    e.transform().position = {1, 2, 3};
    const u64 savedId = e.uuid();
    const auto path = std::filesystem::temp_directory_path() / "nova_editor_test" / "level.scene.json";
    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);

    std::string error;
    CHECK(f.ctx.saveScene(path, &error)); // creates the directory
    CHECK(f.ctx.scenePath() == path && f.scene.name() == "level");

    f.ctx.newScene();
    CHECK(f.scene.registry().view<IdComponent>().empty() && f.ctx.scenePath().empty());

    CHECK(f.ctx.openScene(path, &error));
    Entity loaded = f.scene.findEntity("Saved");
    CHECK(loaded.valid() && loaded.uuid() == savedId);
    if (loaded.valid()) CHECK(loaded.transform().position == glm::vec3(1, 2, 3));
    CHECK(!f.ctx.openScene(path.parent_path() / "missing.scene.json", &error) && !error.empty());
    std::filesystem::remove_all(path.parent_path(), ec);
}

struct Range {
    float lo = 0, hi = 1;
};
struct Spawner {
    Range delay;
    int count = 3;
};

void reflectedInspectors() {
    reflect::type<Range>("TestRange").field("lo", &Range::lo).field("hi", &Range::hi);
    reflect::type<Spawner>("TestSpawner").field("delay", &Spawner::delay).field("count", &Spawner::count).component();
    Fixture f;

    // component<T>() builds the inspector entry from reflection.
    ComponentEditor& ed = f.registry.component<Spawner>();
    CHECK(ed.name == "Test Spawner");
    Entity e = f.scene.createEntity("E");
    CHECK(!ed.has(e));
    ed.add(e);
    CHECK(ed.has(e) && e.get<Spawner>().count == 3);
    ed.remove(e);
    CHECK(!e.has<Spawner>());

    // addReflectedEditors registers the rest once, skips hidden ones (Id, Name) and keeps existing entries.
    f.registry.addReflectedEditors();
    const size_t n = f.registry.componentEditors().size();
    f.registry.addReflectedEditors();
    CHECK(f.registry.componentEditors().size() == n);
    bool hasTransform = false, hasId = false, hasScript = false;
    for (auto& c : f.registry.componentEditors()) {
        hasTransform |= c.name == "Transform";
        hasId |= c.name == "Id";
        hasScript |= c.name.rfind("Script: ", 0) == 0;
    }
    CHECK(hasTransform && !hasId && hasScript);

    // Field drawers: field-specific wins over type-based.
    const reflect::TypeInfo& spawner = *reflect::types().find<Spawner>();
    const reflect::FieldInfo& delay = *spawner.field("delay");
    CHECK(f.registry.findFieldDrawer(spawner, delay) == nullptr);
    f.registry.fieldDrawer<Range>([](Range&, const reflect::FieldInfo&, EditorContext&) { return false; });
    const FieldDrawer* byType = f.registry.findFieldDrawer(spawner, delay);
    CHECK(byType != nullptr);
    f.registry.fieldDrawer("TestSpawner", "delay", [](void*, const reflect::FieldInfo&, EditorContext&) { return true; });
    const FieldDrawer* byField = f.registry.findFieldDrawer(spawner, delay);
    CHECK(byField != nullptr && byField != byType);
    Range r;
    CHECK((*byField)(&r, delay, f.ctx));
}

void signals() {
    Signal<int> s;
    int sum = 0;
    u32 a = s.connect([&](int v) { sum += v; });
    u32 b = 0;
    b = s.connect([&](int v) {
        sum += 10 * v;
        s.disconnect(b); // disconnecting itself during emit is safe
    });
    s.emit(1);
    CHECK(sum == 11 && s.size() == 1);
    s.emit(1);
    CHECK(sum == 12);
    s.disconnect(a);
    s.emit(1);
    CHECK(sum == 12 && s.size() == 0);
}

} // namespace

int main() {
    Log::init();
    Log::core().set_level(spdlog::level::warn);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"key chord parsing", keyChordParsing},
        {"menu path splitting", menuPathSplitting},
        {"commands", commands},
        {"panels", panels},
        {"component editors", componentEditors},
        {"plugins", plugins},
        {"selection", selection},
        {"play / pause / stop", playPauseStop},
        {"stop restores the whole scene", stopRestoresWholeScene},
        {"scene files (new / save / open)", sceneFiles},
        {"reflected inspectors and field drawers", reflectedInspectors},
        {"signals", signals},
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
