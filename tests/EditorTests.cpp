// Headless tests of the editor framework's logic (no window, no GPU, no ImGui frame).
#include <nova/editor/Editor.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
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

// ------------------------------------------------------------------ phase 3 --
void undoStack() {
    UndoStack s;
    int value = 0;
    auto step = [&](int from, int to) { return UndoAction{"set " + std::to_string(to), [&, from] { value = from; }, [&, to] { value = to; }}; };
    value = 1; s.push(step(0, 1));
    value = 2; s.push(step(1, 2));
    CHECK(s.canUndo() && !s.canRedo() && s.undoName() == "set 2");
    CHECK(s.undo() && value == 1 && s.redoName() == "set 2");
    CHECK(s.redo() && value == 2);
    s.jumpTo(0);
    CHECK(value == 0 && s.position() == 0 && !s.undo());
    s.jumpTo(2);
    CHECK(value == 2);
    s.undo();
    value = 5; s.push(step(1, 5)); // new branch drops "set 2"
    CHECK(s.actions().size() == 2 && !s.canRedo() && s.undoName() == "set 5");
    s.setCapacity(1);
    CHECK(s.actions().size() == 1 && s.position() == 1);
    int notified = 0;
    s.changed.connect([&] { ++notified; });
    s.undo();
    s.clear();
    CHECK(notified == 2 && s.actions().empty());
}

void transactions() {
    Fixture f;
    Entity e = f.scene.createEntity("Crate");
    e.transform().position = {1, 0, 0};
    const u64 id = e.uuid();

    // Modification: one step, restored in place (same handle).
    f.ctx.edit("Move Crate", {e}, [&] { e.transform().position = {5, 0, 0}; });
    CHECK(f.ctx.undoStack().position() == 1 && f.ctx.undoStack().undoName() == "Move Crate");
    f.ctx.undo();
    CHECK(e.valid() && e.transform().position == glm::vec3(1, 0, 0));
    f.ctx.redo();
    CHECK(e.transform().position == glm::vec3(5, 0, 0));

    // A no-op edit records nothing.
    f.ctx.edit("Nothing", {e}, [] {});
    CHECK(f.ctx.undoStack().position() == 1);

    // Creation: undo deletes, redo brings back the same UUID with its components.
    auto created = f.ctx.recordCreation("Spawn", [&] {
        Entity a = f.scene.createEntity("A");
        a.add<PointLightComponent>().radius = 3.0f;
        f.scene.createEntity("B");
    });
    CHECK(created.size() == 2 && created[0].name() == "A" && created[1].name() == "B");
    const u64 aId = created[0].uuid();
    f.ctx.undo();
    CHECK(!f.scene.findEntityByUuid(aId).valid() && !f.scene.findEntity("B").valid());
    f.ctx.redo();
    Entity a = f.scene.findEntityByUuid(aId);
    CHECK(a.valid() && a.has<PointLightComponent>() && a.get<PointLightComponent>().radius == 3.0f);

    // Destruction: undo restores everything, including scripts; selection is cleaned.
    reflect::type<Spinner>("TestSpinner").field("speed", &Spinner::speed).script();
    e.addScript<Spinner>().speed = 7.0f;
    f.ctx.select(e);
    f.ctx.destroyEntities("Delete Crate", {e});
    CHECK(!f.scene.findEntityByUuid(id).valid() && f.ctx.selection().empty());
    f.ctx.undo();
    Entity back = f.scene.findEntityByUuid(id);
    CHECK(back.valid() && back.transform().position == glm::vec3(5, 0, 0));
    auto* sc = back.valid() ? back.tryGet<ScriptComponent>() : nullptr;
    CHECK(sc && sc->scripts.size() == 1 && static_cast<Spinner&>(*sc->scripts[0]).speed == 7.0f);
    f.ctx.redo();
    CHECK(!f.scene.findEntityByUuid(id).valid());

    // Interactive transaction (tools): begin, change over several frames, commit once.
    Entity t = f.scene.createEntity("Tool target");
    auto tx = f.ctx.beginEdit("Drag", {t});
    for (int i = 0; i < 5; ++i) t.transform().position.x += 1.0f;
    const size_t before = f.ctx.undoStack().position();
    CHECK(tx.commit() && f.ctx.undoStack().position() == before + 1);
    CHECK(!tx.active() && !tx.commit()); // committing twice does nothing
}

void automaticTracking() {
    Fixture f;
    Entity e = f.scene.createEntity("Lamp");
    f.ctx.select(e);
    f.ctx.updateEditTracking(false); // baseline

    e.transform().position = {0, 3, 0};    // e.g. a slider being dragged...
    f.ctx.updateEditTracking(true);        // ...still held: nothing yet
    e.transform().position = {0, 4, 0};
    f.ctx.updateEditTracking(true);
    CHECK(f.ctx.undoStack().position() == 0);
    f.ctx.updateEditTracking(false);       // released: one step for the whole drag
    CHECK(f.ctx.undoStack().position() == 1 && f.ctx.undoStack().undoName() == "Edit Lamp");
    f.ctx.updateEditTracking(false);       // no further change: nothing new
    CHECK(f.ctx.undoStack().position() == 1);

    f.ctx.undo();
    CHECK(e.transform().position == glm::vec3(0, 0, 0));
    f.ctx.updateEditTracking(false);       // the undo itself is not recorded as an edit
    f.ctx.updateEditTracking(false);
    CHECK(f.ctx.undoStack().position() == 0 && f.ctx.undoStack().canRedo());

    // Changing the selection re-baselines (no step for the other entity's existing state).
    Entity other = f.scene.createEntity("Other");
    other.transform().scale = glm::vec3(3);
    f.ctx.select(other);
    f.ctx.updateEditTracking(false);
    CHECK(f.ctx.undoStack().position() == 0);

    // While playing nothing is recorded (Stop restores the scene anyway).
    f.ctx.play();
    other.transform().position = {7, 7, 7};
    f.ctx.updateEditTracking(false);
    f.ctx.stop();
    f.ctx.updateEditTracking(false);
    CHECK(f.ctx.undoStack().position() == 0);
}

void multiSelection() {
    Fixture f;
    Entity a = f.scene.createEntity("A"), b = f.scene.createEntity("B"), c = f.scene.createEntity("C");
    int changes = 0;
    f.ctx.selectionChanged.connect([&](Entity) { ++changes; });
    f.ctx.select(a);
    f.ctx.addToSelection(b);
    CHECK(f.ctx.selection().size() == 2 && f.ctx.selected() == b && f.ctx.isSelected(a));
    f.ctx.addToSelection(a); // re-adding makes it primary, no duplicate
    CHECK(f.ctx.selection().size() == 2 && f.ctx.selected() == a);
    f.ctx.toggleSelection(c);
    f.ctx.toggleSelection(b);
    CHECK(f.ctx.selection().size() == 2 && f.ctx.isSelected(c) && !f.ctx.isSelected(b));
    f.ctx.setSelection({a, Entity{}, a, b});
    CHECK(f.ctx.selection().size() == 2 && f.ctx.selected() == b); // invalid/duplicates dropped
    f.scene.destroyEntity(b);
    f.ctx.validateSelection();
    CHECK(f.ctx.selection().size() == 1 && f.ctx.selected() == a);
    CHECK(changes == 7); // select, add, re-add (new primary), toggle, toggle, setSelection, validate

    // Stop restores the whole selection by UUID.
    f.ctx.setSelection({a, c});
    const u64 aId = a.uuid(), cId = c.uuid();
    f.ctx.play();
    f.ctx.stop();
    CHECK(f.ctx.selection().size() == 2 && f.ctx.selection()[0].uuid() == aId && f.ctx.selected().uuid() == cId);
}

struct CountingTool : Tool {
    int* activations;
    CountingTool(std::string name, int* a) : Tool(std::move(name), "B"), activations(a) {}
    void onActivate(EditorContext&) override { ++*activations; }
};

void tools() {
    int activations = 0;
    Fixture f;
    f.registry.tool<CountingTool>("First", &activations);
    f.registry.tool<CountingTool>("Second", &activations);
    CHECK(f.ctx.activeTool() == nullptr);
    f.registry.attachAll();
    CHECK(f.ctx.activeTool() == f.registry.findTool("First") && activations == 1); // first tool active
    Command* cmd = f.registry.findCommand("tool.Second");
    CHECK(cmd && cmd->menuPath == "View/Tools/Second" && cmd->shortcut.key == Key::B);
    CHECK(f.registry.execute("tool.Second") && f.ctx.activeTool() == f.registry.findTool("Second") && activations == 2);
    CHECK(cmd->isChecked(f.ctx));
    f.registry.detachAll();
    CHECK(f.ctx.activeTool() == nullptr);
}

void picking() {
    Fixture f;
    const MeshData cube = MeshData::cube();
    MeshLookup meshes = [&](MeshHandle h) { return h.id == 1 ? &cube : nullptr; };

    Entity near = f.scene.createEntity("Near");
    near.transform().position = {0, 0, -5};
    near.add<MeshComponent>(MeshHandle{1});
    Entity far = f.scene.createEntity("Far");
    far.transform().position = {0, 0, -10};
    far.add<MeshComponent>(MeshHandle{1});
    Entity lamp = f.scene.createEntity("Lamp"); // no mesh: picked as a small sphere
    lamp.transform().position = {3, 0, -5};

    auto hit = pick(f.scene, meshes, {0, 0, 0}, {0, 0, -1});
    CHECK(hit && hit->entity == near && std::abs(hit->distance - 4.5f) < 1e-4f && hit->normal.z > 0.99f);
    PickOptions skipNear;
    skipNear.ignore = [&](Entity e) { return e == near; };
    hit = pick(f.scene, meshes, {0, 0, 0}, {0, 0, -1}, skipNear);
    CHECK(hit && hit->entity == far);
    hit = pick(f.scene, meshes, {3, 0, 0}, {0, 0, -1});
    CHECK(hit && hit->entity == lamp && std::abs(hit->distance - 4.7f) < 1e-4f);
    PickOptions noMarkers;
    noMarkers.markerRadius = 0.0f;
    CHECK(!pick(f.scene, meshes, {3, 0, 0}, {0, 0, -1}, noMarkers));
    CHECK(!pick(f.scene, meshes, {0, 0, 0}, {0, 1, 0}));
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

void assets() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "nova_editor_assets_test";
    fs::remove_all(root);
    Fixture f;
    f.ctx.assets().setRoot(root);

    // Prefab from the "selection", then placed through the context (one undo step, selected).
    Entity a = f.scene.createEntity("Pillar");
    a.transform().position = {4, 0, 0};
    Entity b = f.scene.createEntity("Cap");
    b.transform().position = {4, 3, 0};
    auto guid = f.ctx.assets().createPrefab({a, b}, "pillar");
    CHECK(guid.has_value());
    if (!guid) return;
    const size_t before = f.ctx.undoStack().actions().size();
    auto placed = f.ctx.instantiateAsset(*guid, {0, 0, -2});
    CHECK(placed.size() == 2 && f.ctx.selection().size() == 2 && f.ctx.isSelected(placed[1]));
    CHECK(f.ctx.undoStack().actions().size() == before + 1);
    CHECK(f.ctx.undoStack().actions().back().name == "Add pillar.prefab.json");
    const u64 capUuid = placed.size() == 2 ? placed[1].uuid() : 0;
    CHECK(f.scene.findEntityByUuid(capUuid).transform().position == glm::vec3(0, 3, -2));
    f.ctx.undo();
    CHECK(!f.scene.findEntityByUuid(capUuid).valid());
    f.ctx.redo();
    CHECK(f.scene.findEntityByUuid(capUuid).valid());

    // The adjust callback runs inside the same undo step.
    auto lifted = f.ctx.instantiateAsset(*guid, {}, [](std::vector<Entity>& es) {
        for (Entity e : es) e.transform().position.y += 10.0f;
    });
    CHECK(lifted.size() == 2 && lifted[0].transform().position.y == 10.0f);
    CHECK(f.ctx.undoStack().actions().size() == before + 2);
    CHECK(f.ctx.instantiateAsset(12345, {}).empty());

    // Asset actions: type-specific first, then "*"; same type + label replaces.
    int runs = 0;
    f.registry.assetAction("*", "Copy", [&](EditorContext&, const AssetRecord&) { runs += 100; });
    f.registry.assetAction("Prefab", "Place", [&](EditorContext&, const AssetRecord&) { runs += 1; });
    f.registry.assetAction("Prefab", "Place", [&](EditorContext&, const AssetRecord&) { runs += 2; });
    auto actions = f.registry.assetActions("Prefab");
    CHECK(actions.size() == 2 && actions[0]->label == "Place" && actions[1]->label == "Copy");
    actions[0]->run(f.ctx, *f.ctx.assets().find(*guid));
    CHECK(runs == 2);
    CHECK(f.registry.assetActions("Texture").size() == 1);
    CHECK(f.registry.removeAssetAction("Prefab", "Place") && !f.registry.removeAssetAction("Prefab", "Place"));

    // Scenes saved inside the asset folder become assets.
    CHECK(f.ctx.saveScene(root / "levels" / "one.scene.json"));
    const AssetRecord* scene = f.ctx.assets().findByPath("levels/one.scene.json");
    CHECK(scene && scene->type == "Scene");
    fs::remove_all(root);
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
        {"undo stack", undoStack},
        {"undo transactions (edit / create / destroy)", transactions},
        {"automatic undo tracking", automaticTracking},
        {"multi-selection", multiSelection},
        {"tools", tools},
        {"picking", picking},
        {"signals", signals},
        {"assets (prefabs, instantiate + undo, actions)", assets},
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
