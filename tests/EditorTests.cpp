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

    f.ctx.stop();
    CHECK(f.ctx.playState() == PlayState::Edit && !f.scene.isSimulating());
    CHECK(std::abs(box.transform().position.y - 5.0f) < 1e-4f); // restored from the snapshot at play()
    CHECK((states == std::vector<PlayState>{PlayState::Play, PlayState::Pause, PlayState::Play, PlayState::Edit}));

    f.ctx.stop(); // no-op in Edit
    f.ctx.pause();
    CHECK(states.size() == 4);
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
