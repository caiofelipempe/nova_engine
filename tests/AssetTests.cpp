// Headless tests of the asset database: GUID sidecars, importers, references, prefabs, import.
#include <nova/Nova.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

using namespace nova;
namespace fs = std::filesystem;

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

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Fresh, empty folder per test.
fs::path tempRoot(const char* name) {
    fs::path p = fs::temp_directory_path() / "nova_asset_tests" / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// A GPU-free importer whose "resources" are fake handles: lets references be tested headless.
int g_fakeLoads = 0;
AssetImporter fakeImporter() {
    return {
        .type = "Fake",
        .extensions = {".fake"},
        .load = [](AssetDatabase&, const fs::path&, AssetData& out) {
            ++g_fakeLoads;
            out.meshes = {MeshHandle{1001}, MeshHandle{1002}};
            out.textures = {rhi::TextureHandle{77, 3}};
            return true;
        },
        .instantiate = [](AssetDatabase&, const AssetData& data, Scene& scene, const GlobalPosition& position) {
            Entity e = scene.createEntity("Fake");
            e.transform().position = position;
            e.add<MeshComponent>(data.meshes[1]);
            e.get<MeshComponent>().material.baseColorTexture = data.textures[0];
            return std::vector<Entity>{e};
        },
    };
}

// ------------------------------------------------------------------ tests --
void references() {
    const std::string ref = AssetDatabase::makeRef(0x00ab00cd00ef0012ull, 3);
    CHECK(ref == "asset:00ab00cd00ef0012#3");
    AssetGuid g = 0;
    u32 i = 99;
    CHECK(AssetDatabase::parseRef(ref, g, i) && g == 0x00ab00cd00ef0012ull && i == 3);
    CHECK(AssetDatabase::parseRef("asset:ff", g, i) && g == 0xff && i == 0); // index optional
    CHECK(!AssetDatabase::parseRef("Cube", g, i));
    CHECK(!AssetDatabase::parseRef("asset:", g, i));
    CHECK(!AssetDatabase::parseRef("asset:0#1", g, i));          // GUID 0 is invalid
    CHECK(!AssetDatabase::parseRef("asset:xyz#1", g, i));
    CHECK(!AssetDatabase::parseRef("asset:12#a", g, i));
    CHECK(!AssetDatabase::parseRef("asset:12345678901234567#0", g, i)); // > 64 bits
    CHECK(AssetDatabase::newGuid() != 0 && AssetDatabase::newGuid() != AssetDatabase::newGuid());
}

void scanAndMeta() {
    const fs::path root = tempRoot("scan");
    writeFile(root / "textures/wood.png", "not really a png");
    writeFile(root / "models/ship.glb", "x");
    writeFile(root / "prefabs/enemy.prefab.json", "{}");
    writeFile(root / "levels/one.scene.json", "{}");
    writeFile(root / "notes.txt", "ignored");
    writeFile(root / "data.json", "ignored: plain .json has no importer");

    AssetDatabase db;
    db.setRoot(root);
    CHECK(db.assets().size() == 4);
    const AssetRecord* tex = db.findByPath("textures/wood.png");
    const AssetRecord* model = db.findByPath(root / "models" / "ship.glb"); // absolute works too
    const AssetRecord* prefab = db.findByPath("prefabs/enemy.prefab.json");
    const AssetRecord* scene = db.findByPath("levels/one.scene.json");
    CHECK(tex && tex->type == "Texture" && tex->fileName() == "wood.png");
    CHECK(model && model->type == "Model");
    CHECK(prefab && prefab->type == "Prefab");   // compound suffix wins over nothing
    CHECK(scene && scene->type == "Scene" && !scene->importer->load && !db.canInstantiate(*scene));
    CHECK(fs::exists(root / "textures/wood.png.meta") && !fs::exists(root / "notes.txt.meta"));
    CHECK(db.assets().front().path.generic_string() <= db.assets().back().path.generic_string()); // sorted
    const AssetGuid texGuid = tex->guid;

    // GUIDs are stable across sessions (read back from the .meta files).
    AssetDatabase again;
    again.setRoot(root);
    CHECK(again.findByPath("textures/wood.png") && again.findByPath("textures/wood.png")->guid == texGuid);
    CHECK(again.find(texGuid) && again.find(texGuid)->path == fs::path("textures/wood.png"));

    // Moving a file together with its .meta keeps its identity.
    fs::create_directories(root / "art");
    fs::rename(root / "textures/wood.png", root / "art/wood.png");
    fs::rename(root / "textures/wood.png.meta", root / "art/wood.png.meta");
    db.refresh();
    CHECK(db.find(texGuid) && db.find(texGuid)->path == fs::path("art/wood.png"));

    // A copy that kept the .meta gets a new GUID; the original keeps its own.
    fs::copy_file(root / "art/wood.png", root / "art/wood2.png");
    fs::copy_file(root / "art/wood.png.meta", root / "art/wood2.png.meta");
    db.refresh();
    const AssetRecord* copy = db.findByPath("art/wood2.png");
    CHECK(copy && copy->guid != texGuid && db.find(texGuid) && db.find(texGuid)->fileName() == "wood.png");

    // Deleted files disappear; a broken .meta is regenerated.
    fs::remove(root / "art/wood2.png");
    writeFile(root / "models/ship.glb.meta", "garbage");
    db.refresh();
    CHECK(!db.findByPath("art/wood2.png") && db.assets().size() == 4);
    CHECK(db.findByPath("models/ship.glb") && db.findByPath("models/ship.glb")->guid != 0);
}

void importersAndLoading() {
    const fs::path root = tempRoot("importers");
    writeFile(root / "a.fake", "");
    writeFile(root / "b.dlg.json", "{\"lines\": 3}");
    AssetDatabase db;
    db.setRoot(root);
    CHECK(db.assets().empty()); // no importer for .fake yet

    db.addImporter(fakeImporter()); // registering rescans
    db.addImporter({.type = "Dialogue",
                    .extensions = {".DLG.json"}, // case-insensitive
                    .load = [](AssetDatabase&, const fs::path& file, AssetData& out) {
                        out.payload = std::make_shared<std::string>(readFile(file));
                        return true;
                    }});
    CHECK(db.assets().size() == 2);
    CHECK(db.importerFor("x/Y.FAKE") && db.importerFor("x/Y.FAKE")->type == "Fake");
    CHECK(db.importerFor("level.scene.json")->type == "Scene"); // longest suffix
    CHECK(!db.importerFor("x.json") && !db.importerFor(".fake"));

    const AssetRecord* fake = db.findByPath("a.fake");
    CHECK(fake && !db.isLoaded(fake->guid));
    g_fakeLoads = 0;
    const AssetData* data = db.load(fake->guid);
    CHECK(data && data->meshes.size() == 2 && db.isLoaded(fake->guid));
    CHECK(db.load(fake->guid) == data && g_fakeLoads == 1); // cached
    db.unload(fake->guid);
    CHECK(db.load(fake->guid) && g_fakeLoads == 2);

    const AssetRecord* dlg = db.findByPath("b.dlg.json");
    const AssetData* dd = dlg ? db.load(dlg->guid) : nullptr;
    CHECK(dd && dd->as<std::string>() && dd->as<std::string>()->find("lines") != std::string::npos);

    const AssetGuid fakeGuid = fake->guid, dlgGuid = dlg->guid; // records are rebuilt by refresh()

    // GPU assets without a renderer fail cleanly (once).
    writeFile(root / "t.png", "x");
    db.refresh();
    const AssetRecord* png = db.findByPath("t.png");
    CHECK(png && !db.load(png->guid) && !db.load(png->guid) && !db.isLoaded(png->guid));
    CHECK(!db.load(0) && !db.load(12345));

    // Instantiation tags entities with their source asset.
    Scene scene;
    auto created = db.instantiate(fakeGuid, scene, {1, 2, 3});
    CHECK(created.size() == 1);
    if (created.size() == 1) {
        CHECK(created[0].transform().position == glm::vec3(1, 2, 3));
        CHECK(created[0].has<PrefabInstanceComponent>() && created[0].get<PrefabInstanceComponent>().asset == fakeGuid);
    }
    CHECK(db.instantiate(dlgGuid, scene).empty()); // no instantiate callback
}

void sceneReferences() {
    const fs::path root = tempRoot("refs");
    writeFile(root / "thing.fake", "");
    AssetDatabase db;
    db.addImporter(fakeImporter());
    db.setRoot(root);
    const AssetGuid guid = db.findByPath("thing.fake")->guid;

    Scene scene;
    Entity e = db.instantiate(guid, scene).front();
    CHECK(db.meshRef(MeshHandle{1002}) == AssetDatabase::makeRef(guid, 1));
    CHECK(db.textureRef(rhi::TextureHandle{77, 3}) == AssetDatabase::makeRef(guid, 0));
    CHECK(db.meshRef(MeshHandle{5}).empty() && db.textureRef(rhi::TextureHandle{1, 1}).empty());
    CHECK(db.textureLabel(rhi::TextureHandle{77, 3}) == "thing.fake #0");

    const std::string json = serializeScene(scene, db.serializeOptions());
    CHECK(json.find(AssetDatabase::makeRef(guid, 1)) != std::string::npos);
    CHECK(json.find(AssetDatabase::makeRef(guid, 0)) != std::string::npos);
    CHECK(json.find("\"PrefabInstance\"") != std::string::npos);

    // A new session resolves the references by loading the asset on demand.
    AssetDatabase fresh;
    fresh.addImporter(fakeImporter());
    fresh.setRoot(root);
    Scene loaded;
    CHECK(deserializeScene(loaded, json, fresh.serializeOptions()));
    Entity back = loaded.findEntityByUuid(e.uuid());
    CHECK(back.valid() && back.has<MeshComponent>());
    CHECK(back.get<MeshComponent>().mesh == MeshHandle{1002});
    CHECK(back.get<MeshComponent>().material.baseColorTexture == (rhi::TextureHandle{77, 3}));
    CHECK(fresh.isLoaded(guid));

    // Unknown references load as empty handles; meshes not from assets fall back to ids.
    Scene other;
    Entity plain = other.createEntity("Plain");
    plain.add<MeshComponent>(MeshHandle{9});
    const std::string plainJson = serializeScene(other, db.serializeOptions());
    CHECK(plainJson.find("\"mesh\": 9") != std::string::npos);
    const std::string missing = "{\"format\":\"nova.scene\",\"version\":1,\"entities\":[{\"components\":{\"Mesh\":"
                                "{\"mesh\":\"asset:00000000000000aa#0\",\"material\":{\"baseColorTexture\":\"asset:bb#0\"}}}}]}";
    Scene broken;
    CHECK(deserializeScene(broken, missing, fresh.serializeOptions()));
    bool emptyHandles = false;
    for (auto [h, m] : broken.registry().view<MeshComponent>().each()) emptyHandles = !m.mesh && !m.material.baseColorTexture;
    CHECK(emptyHandles);
}

void prefabs() {
    const fs::path root = tempRoot("prefabs");
    AssetDatabase db;
    db.setRoot(root);
    CHECK(db.animationNames(12345).empty()); // unknown: no clips (the inspector lists none)

    Scene scene;
    Entity a = scene.createEntity("Base");
    a.transform().position = {10, 0, 5};
    a.add<RotatorComponent>(glm::vec3(0, 1, 0), AngleSpeed::fromRadiansPerSecond(2.5));
    Entity b = scene.createEntity("Top");
    b.transform().position = {10, 2, 5};
    b.transform().scale = glm::vec3(0.5f);

    std::string error;
    CHECK(!db.createPrefab({}, "empty", &error) && !error.empty());
    auto guid = db.createPrefab({a, b}, "props/tower", &error);
    CHECK(guid.has_value());
    CHECK(fs::exists(root / "props/tower.prefab.json") && fs::exists(root / "props/tower.prefab.json.meta"));
    const AssetRecord* rec = guid ? db.find(*guid) : nullptr;
    CHECK(rec && rec->type == "Prefab" && db.canInstantiate(*rec));

    const std::string text = readFile(root / "props/tower.prefab.json");
    CHECK(text.find("nova.prefab") != std::string::npos && text.find("\"Id\"") == std::string::npos);

    Scene level;
    auto first = db.instantiate(*guid, level, {0, 1, 0});
    auto second = db.instantiate(*guid, level, {-4, 0, 0});
    CHECK(first.size() == 2 && second.size() == 2);
    if (first.size() == 2 && second.size() == 2) {
        CHECK(first[0].name() == "Base" && first[1].name() == "Top");
        CHECK(first[0].transform().position == glm::vec3(0, 1, 0));  // pivot = first entity
        CHECK(first[1].transform().position == glm::vec3(0, 3, 0));  // relative offset kept
        CHECK(second[1].transform().position == glm::vec3(-4, 2, 0));
        CHECK(first[1].transform().scale == glm::vec3(0.5f));
        CHECK(first[0].has<RotatorComponent>() && first[0].get<RotatorComponent>().speed == AngleSpeed::fromRadiansPerSecond(2.5));
    CHECK(db.animationNames(*guid).empty()); // a prefab has no clips
        CHECK(first[0].uuid() != second[0].uuid() && first[0].uuid() != a.uuid()); // fresh UUIDs
        CHECK(first[0].get<PrefabInstanceComponent>().asset == *guid);
    }

    // Saving again over the same file updates the asset (same GUID, reloaded data).
    a.transform().position = {0, 0, 0};
    b.transform().position = {0, 5, 0};
    auto again = db.createPrefab({a, b}, "props/tower.prefab.json");
    CHECK(again && *again == *guid);
    auto third = db.instantiate(*guid, level);
    CHECK(third.size() == 2 && third[1].transform().position == glm::vec3(0, 5, 0));

    // Engine-level API without a database.
    Scene s2;
    auto direct = instantiatePrefab(s2, serializePrefab({b}), {1, 1, 1});
    CHECK(direct.size() == 1 && direct[0].transform().position == glm::vec3(1, 1, 1));
    error.clear();
    CHECK(instantiatePrefab(s2, "{\"format\":\"nova.scene\"}", {}, {}, &error).empty() && !error.empty());
}

void importFiles() {
    const fs::path root = tempRoot("import");
    const fs::path outside = tempRoot("import_src");
    AssetDatabase db;
    db.setRoot(root);

    std::string error;
    CHECK(!db.import(outside / "missing.png", {}, &error) && !error.empty());
    writeFile(outside / "readme.txt", "x");
    CHECK(!db.import(outside / "readme.txt", {}, &error)); // no importer

    // A .gltf brings the files it references.
    writeFile(outside / "ship/ship.gltf",
              R"({"asset":{"version":"2.0"},"buffers":[{"uri":"ship%20data.bin","byteLength":4}],)"
              R"("images":[{"uri":"tex/hull.png"},{"uri":"data:image/png;base64,AAAA"}]})");
    writeFile(outside / "ship/ship data.bin", "1234");
    writeFile(outside / "ship/tex/hull.png", "png");
    const auto deps = gltfDependencies(outside / "ship/ship.gltf");
    CHECK(deps.size() == 2); // the data: URI is embedded
    auto guid = db.import(outside / "ship/ship.gltf", "models/ship", &error);
    CHECK(guid.has_value());
    CHECK(fs::exists(root / "models/ship/ship.gltf") && fs::exists(root / "models/ship/ship data.bin"));
    CHECK(fs::exists(root / "models/ship/tex/hull.png"));
    CHECK(guid && db.find(*guid) && db.find(*guid)->path == fs::path("models/ship/ship.gltf"));
    CHECK(db.findByPath("models/ship/tex/hull.png") != nullptr); // the image is an asset too

    // Files already inside the root are registered, not copied.
    writeFile(root / "inside.png", "x");
    auto inside = db.import(root / "inside.png", "elsewhere");
    CHECK(inside && db.find(*inside)->path == fs::path("inside.png") && !fs::exists(root / "elsewhere/inside.png"));
}

} // namespace

void modelHierarchy() {
    // A model keeps its node tree: a root named after it, each node under its parent, each part at
    // its pose inside the model; moving the root moves it all.
    ModelData model;
    model.name = "cart";
    model.meshes = {MeshHandle{1}, MeshHandle{2}};
    ModelNode body;
    body.name = "Body";
    body.position = {0, 1, 0};
    body.mesh = 0;
    ModelNode axle; // a transform only
    axle.name = "Axle";
    axle.position = {0, 0.5f, 1};
    axle.parent = 0;
    ModelNode wheel;
    wheel.name = "Wheel";
    wheel.position = {1, 0.5f, 1};
    wheel.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    wheel.mesh = 1;
    wheel.parent = 1;
    ModelNode lamp; // another top-level node
    lamp.name = "Lamp";
    lamp.position = {0, 3, 0};
    lamp.mesh = 1;
    model.nodes = {body, axle, wheel, lamp};

    Scene scene;
    auto made = instantiateModel(scene, model, GlobalPosition(10, 0, 0));
    CHECK(made.size() == 5 && made[0].name() == "cart" && !made[0].parent().valid());
    Entity root = made[0], bodyE = made[1], axleE = made[2], wheelE = made[3], lampE = made[4];
    CHECK(bodyE.parent() == root && axleE.parent() == bodyE && wheelE.parent() == axleE && lampE.parent() == root);
    CHECK(bodyE.has<MeshComponent>() && !axleE.has<MeshComponent>() && wheelE.get<MeshComponent>().mesh.id == 2);
    CHECK(wheelE.transform().position == GlobalPosition(11, 0.5, 1)); // its pose in the model, at the root
    CHECK(std::abs(std::abs(glm::dot(wheelE.transform().orientation(), wheel.rotation)) - 1.0f) < 1e-5f);
    root.transform().position = {20, 0, 0};
    root.transform().setRotation(glm::angleAxis(glm::radians(180.0f), glm::vec3(0, 1, 0)));
    scene.updateTransforms();
    CHECK(glm::length((wheelE.transform().position - GlobalPosition(19, 0.5, -1)).toMeters()) < 1e-4);
    CHECK(glm::length((lampE.transform().position - GlobalPosition(20, 3, 0)).toMeters()) < 1e-4);
}

void animationSampling() {
    // Linear, step and cubic channels, and rotations by slerp.
    AnimationChannel move;
    move.times = {0.0f, 2.0f};
    move.values = {glm::vec4(0, 0, 0, 0), glm::vec4(4, 2, 0, 0)};
    CHECK(move.sample(1.0f) == glm::vec4(2, 1, 0, 0) && move.sample(-1.0f) == glm::vec4(0.0f) && move.sample(9.0f) == glm::vec4(4, 2, 0, 0));
    move.interpolation = AnimationInterpolation::Step;
    CHECK(move.sample(1.9f) == glm::vec4(0.0f) && move.sample(2.0f) == glm::vec4(4, 2, 0, 0));
    AnimationChannel turn;
    turn.path = AnimationPath::Rotation;
    turn.times = {0.0f, 1.0f};
    const glm::quat a(1, 0, 0, 0), b = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    turn.values = {glm::vec4(a.x, a.y, a.z, a.w), glm::vec4(b.x, b.y, b.z, b.w)};
    const glm::vec4 half = turn.sample(0.5f);
    const glm::quat q(half.w, half.x, half.y, half.z), want = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0));
    CHECK(std::abs(std::abs(glm::dot(q, want)) - 1.0f) < 1e-5f);
    // Cubic spline with flat tangents: eases in and out, through the keys.
    AnimationChannel ease;
    ease.interpolation = AnimationInterpolation::CubicSpline;
    ease.times = {0.0f, 1.0f};
    ease.values = {glm::vec4(0), glm::vec4(0), glm::vec4(0), glm::vec4(0), glm::vec4(1, 0, 0, 0), glm::vec4(0)};
    CHECK(std::abs(ease.sample(0.5f).x - 0.5f) < 1e-6f && ease.sample(0.25f).x < 0.25f && ease.sample(1.0f).x == 1.0f);
}

void nodeAnimation() {
    // A model with a clip: its root plays it on the nodes, as offsets from their parents.
    ModelData model;
    model.name = "door";
    ModelNode frame;
    frame.name = "Frame";
    ModelNode hinge;
    hinge.name = "Hinge";
    hinge.position = {1, 0, 0};
    hinge.parent = 0;
    model.nodes = {frame, hinge};
    auto clip = std::make_shared<AnimationClip>();
    clip->name = "open";
    clip->duration = 2.0f;
    AnimationChannel swing;
    swing.target = "Frame/Hinge";
    swing.path = AnimationPath::Rotation;
    swing.times = {0.0f, 2.0f};
    const glm::quat closed(1, 0, 0, 0), open = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    swing.values = {glm::vec4(closed.x, closed.y, closed.z, closed.w), glm::vec4(open.x, open.y, open.z, open.w)};
    AnimationChannel slide; // the frame slides up
    slide.target = "Frame";
    slide.times = {0.0f, 2.0f};
    slide.values = {glm::vec4(0, 0, 0, 0), glm::vec4(0, 2, 0, 0)};
    clip->channels = {swing, slide};
    model.animations = {clip};

    Scene scene;
    auto made = instantiateModel(scene, model, GlobalPosition(10, 0, 0));
    Entity root = made[0], frameE = made[1], hingeE = made[2];
    CHECK(root.has<AnimatorComponent>() && root.get<AnimatorComponent>().data == clip);
    auto angleOf = [](Entity e) { return glm::degrees(glm::angle(e.transform().orientation())); };
    scene.update(Duration::fromSeconds(1.0));
    CHECK(std::abs(angleOf(hingeE) - 45.0f) < 0.01f);
    CHECK(glm::length((frameE.transform().position - GlobalPosition(10, 1, 0)).toMeters()) < 1e-4);
    CHECK(glm::length((hingeE.transform().position - GlobalPosition(11, 1, 0)).toMeters()) < 1e-4); // follows the frame
    // Moving the model's root moves the animation with it.
    root.transform().position = {20, 0, 0};
    scene.update(Duration::fromSeconds(0.5));
    CHECK(glm::length((frameE.transform().position - GlobalPosition(20, 1.5, 0)).toMeters()) < 1e-4);
    // Loops; paused holds; faster.
    scene.update(Duration::fromSeconds(1.0)); // 2.5 s ≡ 0.5 s
    CHECK(std::abs(root.get<AnimatorComponent>().time.seconds() - 0.5) < 1e-9 && std::abs(angleOf(hingeE) - 22.5f) < 0.01f);
    root.get<AnimatorComponent>().playing = false;
    scene.update(Duration::fromSeconds(1.0));
    CHECK(std::abs(angleOf(hingeE) - 22.5f) < 0.01f);
    root.get<AnimatorComponent>().playing = true;
    root.get<AnimatorComponent>().speed = 2;
    scene.update(Duration::fromSeconds(0.25)); // +0.5 s of clip
    CHECK(std::abs(angleOf(hingeE) - 45.0f) < 0.01f);
    root.get<AnimatorComponent>().loop = false;
    scene.update(Duration::fromSeconds(5.0));
    CHECK(std::abs(angleOf(hingeE) - 90.0f) < 0.01f); // holds the last pose

    // Saved by reference: a loaded scene finds the clip through its animation source.
    root.get<AnimatorComponent>().clip = "test:open";
    const std::string json = serializeScene(scene);
    Scene loaded;
    loaded.setAnimationSource([&](const std::string& ref) { return ref == "test:open" ? clip : nullptr; });
    CHECK(deserializeScene(loaded, json));
    Entity root2 = loaded.findEntity("door");
    CHECK(root2.has<AnimatorComponent>() && root2.get<AnimatorComponent>().clip == "test:open" && !root2.get<AnimatorComponent>().data);
    CHECK(root2.get<AnimatorComponent>().speed == 2 && !root2.get<AnimatorComponent>().loop);
    root2.get<AnimatorComponent>().loop = true;
    root2.get<AnimatorComponent>().speed = 1;
    loaded.update(Duration::fromSeconds(1.0)); // from the start
    CHECK(std::abs(glm::degrees(glm::angle(loaded.findEntity("Hinge").transform().orientation())) - 45.0f) < 0.01f);
}

void gltfAnimation() {
    // A glTF file with a node tree and an animation (read without a renderer: nodes and clips).
    const fs::path dir = fs::temp_directory_path() / "nova_gltf_anim";
    fs::create_directories(dir);
    const fs::path file = dir / "door.gltf";
    {
        std::ofstream out(file);
        out << R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "Door", "translation": [0, 1, 0], "children": [1]}, {"name": "Hinge", "translation": [1, 0, 0]}],
  "buffers": [{"byteLength": 40, "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAPMENT8AAAAA8wQ1Pw=="}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 8}, {"buffer": 0, "byteOffset": 8, "byteLength": 32}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
                {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC4"}],
  "animations": [{"name": "Open", "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}],
                  "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}]}]
})";
    }
    auto model = loadModel(nullptr, file);
    CHECK(model.has_value());
    if (!model) return;
    CHECK(model->nodes.size() == 2 && model->nodes[1].parent == 0 && model->nodes[1].mesh == ~0u);
    CHECK(model->animations.size() == 1 && model->animations[0]->name == "Open" && model->animations[0]->duration == 1.0f);
    CHECK(model->animations[0]->channels.size() == 1 && model->animations[0]->channels[0].target == "Door/Hinge");
    Scene scene;
    auto made = instantiateModel(scene, *model);
    scene.update(Duration::fromSeconds(0.5));
    CHECK(std::abs(glm::degrees(glm::angle(made[2].transform().orientation())) - 45.0f) < 0.01f);
    CHECK(glm::length((made[2].transform().position - GlobalPosition(1, 1, 0)).toMeters()) < 1e-4);
    fs::remove_all(dir);
}

void skinning() {
    // A bone 1 m up turns 90° around Z: a vertex 1 m above it (bound to it) swings to its side.
    // The joint matrices are worked out relative to the mesh: the same far out in the world.
    for (const f64 far : {0.0, 1.0e6}) {
        Scene scene;
        Entity root = scene.createEntity("Rig");
        root.transform().position = {far, 0, 0};
        Entity bone = scene.createEntity("Bone");
        bone.transform().position = {far, 1, 0};
        bone.setParent(root);
        Entity body = scene.createEntity("Body");
        body.transform().position = {far, 0, 0};
        body.add<MeshComponent>(MeshHandle{1});
        body.setParent(root);
        auto skin = std::make_shared<SkinData>();
        skin->joints = {"Bone"};
        skin->inverseBind = {glm::translate(glm::mat4(1.0f), glm::vec3(0, -1, 0))}; // the bone sat 1 m up
        auto& sc = body.add<SkinComponent>();
        sc.root = Scene::refOf(root);
        sc.data = skin;

        RenderScene rs;
        const CameraData eye = CameraData::relativeLookAt(GlobalPosition(far, 1, 10), {0, 0, -1}, 1.0f, 1.0f, 0.1f, 100.0f);
        scene.buildRenderScene(rs, 1.0f, &eye);
        CHECK(rs.draws.size() == 1 && rs.draws[0].jointCount == 1 && rs.joints.size() == 1);
        CHECK(rs.draws[0].skinKey == body.uuid()); // the same from frame to frame: its ray tracing shape is kept
        const glm::vec4 rest = rs.joints[0] * glm::vec4(0, 2, 0, 1);
        CHECK(glm::length(glm::vec3(rest) - glm::vec3(0, 2, 0)) < 1e-5f); // the bind pose: unmoved

        bone.transform().setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)));
        scene.buildRenderScene(rs, 1.0f, &eye);
        const glm::vec4 bent = rs.joints[0] * glm::vec4(0, 2, 0, 1);
        CHECK(glm::length(glm::vec3(bent) - glm::vec3(-1, 1, 0)) < 1e-5f);
        // The model moves and turns: the skin goes with it (mesh space matrices do not change).
        root.transform().position = {far + 5, 0, 0};
        root.transform().setRotation(glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0)));
        scene.buildRenderScene(rs, 1.0f, &eye);
        CHECK(glm::length(glm::vec3(rs.joints[0] * glm::vec4(0, 2, 0, 1)) - glm::vec3(-1, 1, 0)) < 1e-4f);

        // Without its skeleton: drawn rigid.
        sc.data.reset();
        scene.buildRenderScene(rs, 1.0f, &eye);
        CHECK(rs.draws[0].jointCount == 0);
    }
}

void gltfSkin() {
    // A glTF skeleton: joints by their path of names, inverse bind matrices, the skinned node.
    const fs::path dir = fs::temp_directory_path() / "nova_gltf_skin";
    fs::create_directories(dir);
    const fs::path file = dir / "arm.gltf";
    {
        std::ofstream out(file);
        out << R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 2]}],
  "nodes": [{"name": "Shoulder", "children": [1]}, {"name": "Elbow", "translation": [0, 1, 0]},
            {"name": "Sleeve", "mesh": 0, "skin": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 1}}]}],
  "skins": [{"joints": [0, 1], "inverseBindMatrices": 0}],
  "buffers": [{"byteLength": 128, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAACAPwAAgD8AAAAAAAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIC/AAAAAAAAgD8="}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 128}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 2, "type": "MAT4"},
                {"bufferView": 0, "componentType": 5126, "count": 1, "type": "VEC3", "min": [1, 0, 0], "max": [1, 0, 0]}]
})";
    }
    auto model = loadModel(nullptr, file); // no renderer: nodes, clips and skins only
    CHECK(model.has_value());
    if (!model) return;
    CHECK(model->skins.size() == 1 && model->skins[0]->joints.size() == 2);
    CHECK(model->skins[0]->joints[0] == "Shoulder" && model->skins[0]->joints[1] == "Shoulder/Elbow");
    CHECK(model->skins[0]->inverseBind[1] == glm::translate(glm::mat4(1.0f), glm::vec3(0, -1, 0)));
    CHECK(model->nodes.size() == 3 && model->nodes[2].name == "Sleeve" && model->nodes[2].skin == 0);
    fs::remove_all(dir);
}

// A clip holding one value on the node "Node": x for a translation, an angle around Y for a rotation.
std::shared_ptr<AnimationClip> holdClip(const char* name, f32 value, bool rotation = false, f32 duration = 1.0f) {
    auto clip = std::make_shared<AnimationClip>();
    clip->name = name;
    clip->duration = duration;
    AnimationChannel ch;
    ch.target = "Node";
    ch.times = {0.0f, duration};
    if (rotation) {
        ch.path = AnimationPath::Rotation;
        const glm::quat q = glm::angleAxis(glm::radians(value), glm::vec3(0, 1, 0));
        ch.values = {glm::vec4(q.x, q.y, q.z, q.w), glm::vec4(q.x, q.y, q.z, q.w)};
    } else {
        ch.values = {glm::vec4(value, 0, 0, 0), glm::vec4(value, 0, 0, 0)};
    }
    clip->channels = {ch};
    return clip;
}

void animationMixing() {
    Scene scene;
    Entity root = scene.createEntity("Root");
    Entity node = scene.createEntity("Node");
    node.setParent(root);
    auto x = [&] { return node.transform().position.toMeters().x; };
    auto idle = holdClip("idle", 0.0f), walk = holdClip("walk", 10.0f);

    // Changing clips at once, then with a cross-fade: halfway, halfway between.
    scene.playAnimation(root, idle);
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(x()) < 1e-6);
    scene.playAnimation(root, walk, Duration::seconds(1));
    scene.update(Duration::fromSeconds(0.5));
    CHECK(std::abs(x() - 5.0) < 1e-4 && root.get<AnimatorComponent>().layers.size() == 1);
    scene.update(Duration::fromSeconds(0.6));
    CHECK(std::abs(x() - 10.0) < 1e-4 && root.get<AnimatorComponent>().layers.empty()); // the old clip is gone
    scene.playAnimation(root, idle); // no fade: at once
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(x()) < 1e-6);

    // Mixing by weight (relative to the main clip's 1): a quarter of walk in idle = 2 m.
    scene.blendAnimation(root, walk, 0.25f);
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(x() - 2.0) < 1e-4);
    scene.blendAnimation(root, walk, 1.0f); // a new weight: half and half
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(x() - 5.0) < 1e-4 && root.get<AnimatorComponent>().layers.size() == 1);
    scene.blendAnimation(root, walk, 0.0f); // removed
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(x()) < 1e-6 && root.get<AnimatorComponent>().layers.empty());

    // Rotations mix as quaternions: 0° and 90° in equal parts make 45°.
    auto aside = holdClip("aside", 90.0f, true);
    scene.playAnimation(root, holdClip("ahead", 0.0f, true));
    scene.blendAnimation(root, aside, 1.0f);
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(glm::degrees(glm::angle(node.transform().orientation())) - 45.0f) < 0.01f);
    // Mixed-in clips stay mixed in when the main clip changes, until their weight goes to 0.
    scene.playAnimation(root, idle);
    CHECK(root.get<AnimatorComponent>().layers.size() == 1);
    scene.blendAnimation(root, aside, 0.0f);

    // In step: a synced layer follows the main clip's phase whatever its own length.
    auto slow = holdClip("slow", 0.0f, false, 2.0f), fast = holdClip("fast", 0.0f, false, 0.5f);
    scene.playAnimation(root, slow);
    scene.blendAnimation(root, fast, 1.0f, true);
    scene.update(Duration::fromSeconds(0.5)); // a quarter of the main clip
    const auto& anim = root.get<AnimatorComponent>();
    CHECK(anim.time == Duration::milliseconds(500) && anim.layers[0].time == Duration::milliseconds(125));

    scene.blendAnimation(root, fast, 0.0f);

    // By reference: the clip the animator saves.
    scene.setAnimationSource([&](const std::string& ref) { return ref == "test:walk" ? walk : nullptr; });
    scene.playAnimation(root, std::string("test:walk"), Duration::milliseconds(200));
    CHECK(root.get<AnimatorComponent>().clip == "test:walk" && root.get<AnimatorComponent>().data == walk);
    scene.update(Duration::fromSeconds(1.0));
    CHECK(std::abs(x() - 10.0) < 1e-4);
}

void boneMasks() {
    // A body (Hips) with an arm (Hips/Arm). Walking: hips at x = 10, arm at x = 1. Waving: arm at
    // x = 5 (and hips at x = −10, which the mask keeps out).
    auto clip2 = [](const char* name, f32 hips, f32 arm, bool rotation = false) {
        auto clip = std::make_shared<AnimationClip>();
        clip->name = name;
        clip->duration = 1.0f;
        for (auto [target, value] : {std::pair{"Hips", hips}, std::pair{"Hips/Arm", arm}}) {
            AnimationChannel ch;
            ch.target = target;
            ch.times = {0.0f, 1.0f};
            if (rotation) {
                ch.path = AnimationPath::Rotation;
                const glm::quat q = glm::angleAxis(glm::radians(value), glm::vec3(0, 1, 0));
                ch.values = {glm::vec4(q.x, q.y, q.z, q.w), glm::vec4(q.x, q.y, q.z, q.w)};
            } else {
                ch.values = {glm::vec4(value, 0, 0, 0), glm::vec4(value, 0, 0, 0)};
            }
            clip->channels.push_back(ch);
        }
        return clip;
    };
    Scene scene;
    Entity root = scene.createEntity("Root");
    Entity hips = scene.createEntity("Hips");
    hips.setParent(root);
    Entity arm = scene.createEntity("Arm");
    arm.setParent(hips);
    auto localX = [&](Entity e) { return toDouble(scene.localTransform(e).position.x); };
    auto walk = clip2("walk", 10.0f, 1.0f), wave = clip2("wave", -10.0f, 5.0f);
    scene.playAnimation(root, walk);

    // Override on the arm only: the arm waves, the hips walk.
    AnimationMix upper;
    upper.mode = AnimationBlend::Override;
    upper.mask = {"Hips/Arm"};
    scene.blendAnimation(root, wave, upper);
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(localX(hips) - 10.0) < 1e-6 && std::abs(localX(arm) - 5.0) < 1e-6);
    upper.weight = 0.5f; // half way between walking and waving
    scene.blendAnimation(root, wave, upper);
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(localX(arm) - 3.0) < 1e-6 && std::abs(localX(hips) - 10.0) < 1e-6);

    // Averaged in with a mask: only the masked nodes take part.
    AnimationMix averaged;
    averaged.mask = {"Hips/Arm"};
    scene.blendAnimation(root, wave, averaged);
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(localX(arm) - 3.0) < 1e-6 && std::abs(localX(hips) - 10.0) < 1e-6);

    // A mask covers its node and everything under it: "Hips" takes the arm along.
    AnimationMix whole;
    whole.mode = AnimationBlend::Override;
    whole.mask = {"Hips"};
    scene.blendAnimation(root, wave, whole);
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(localX(hips) + 10.0) < 1e-6 && std::abs(localX(arm) - 5.0) < 1e-6);
    CHECK(inMask({"Hips"}, "Hips/Arm") && !inMask({"Hips/Ar"}, "Hips/Arm") && !inMask({"Hips/Arm"}, "Hips") && inMask({}, "Any"));
    scene.blendAnimation(root, wave, 0.0f);

    // Rotations override by slerp: 0° walking, 90° waving, half way = 45°.
    scene.playAnimation(root, clip2("ahead", 0.0f, 0.0f, true));
    AnimationMix turn;
    turn.mode = AnimationBlend::Override;
    turn.weight = 0.5f;
    turn.mask = {"Hips/Arm"};
    scene.blendAnimation(root, clip2("aside", 90.0f, 90.0f, true), turn);
    scene.update(Duration::fromSeconds(0.1));
    CHECK(std::abs(glm::degrees(glm::angle(scene.localTransform(arm).rotation.toGlm())) - 45.0f) < 0.01f);
    CHECK(std::abs(glm::degrees(glm::angle(scene.localTransform(hips).rotation.toGlm()))) < 0.01f);
}

struct EventLog : Script {
    std::vector<std::string> seen; // "name value"
    void onAnimationEvent(const AnimationEventHit& e) override {
        seen.push_back(e.name + " " + std::to_string(static_cast<int>(e.value)));
    }
};

void animationEvents() {
    auto clip = std::make_shared<AnimationClip>();
    clip->name = "walk";
    clip->duration = 1.0f;
    clip->events = {{0.75f, "step", 1.0f}, {0.0f, "start"}, {0.25f, "step", 0.0f}}; // any order
    Scene scene;
    Entity e = scene.createEntity("Walker");
    auto& log = e.addScript<EventLog>();
    auto seen = [&] {
        std::string all;
        for (const std::string& s : log.seen) all += (all.empty() ? "" : ", ") + s;
        log.seen.clear();
        return all;
    };
    scene.playAnimation(e, clip);
    scene.update(Duration::fromSeconds(0.5)); // from the start: start, the first step
    CHECK(seen() == "start 0, step 0");
    CHECK(scene.animationEvents().size() == 2 && scene.animationEvents()[0].first == e &&
          scene.animationEvents()[1].second.clip == "walk" && scene.animationEvents()[1].second.time == 0.25f);
    scene.update(Duration::fromSeconds(0.5)); // to the end, which is the next lap's start
    CHECK(seen() == "step 1, start 0" && scene.animationEvents().size() == 2);
    scene.update(Duration::fromSeconds(0.3)); // on into the next lap
    CHECK(seen() == "step 0");
    scene.update(Duration::fromSeconds(0.0));
    CHECK(seen().empty() && scene.animationEvents().empty());

    // Twice as fast for a second: two laps, in order.
    e.get<AnimatorComponent>().speed = 2;
    scene.update(Duration::fromSeconds(1.0)); // from 0.3 to 2.3
    CHECK(seen() == "step 1, start 0, step 0, step 1, start 0, step 0");
    // Backwards: in reverse order.
    e.get<AnimatorComponent>().speed = -1;
    scene.update(Duration::fromSeconds(0.5)); // from 0.3 back to −0.2 ≡ 0.8
    CHECK(seen() == "step 0, start 0");

    // Without looping: once to the end, then nothing while it holds there.
    e.get<AnimatorComponent>().speed = 1;
    scene.playAnimation(e, clip, Duration(), false);
    scene.update(Duration::fromSeconds(2.0));
    CHECK(seen() == "start 0, step 0, step 1");
    scene.update(Duration::fromSeconds(1.0));
    CHECK(seen().empty());

    // A clip fading out stays quiet; a mixed-in one fires only when asked to.
    auto other = std::make_shared<AnimationClip>(*clip);
    other->name = "run";
    other->events = {{0.5f, "breath"}};
    scene.playAnimation(e, clip);
    scene.update(Duration::fromSeconds(0.1));
    seen();
    scene.playAnimation(e, other, Duration::seconds(1)); // walk fades out: its steps are not heard
    scene.update(Duration::fromSeconds(0.6));
    CHECK(seen() == "breath 0");
    AnimationMix loud;
    loud.events = true;
    loud.sync = false;
    scene.blendAnimation(e, clip, loud);
    scene.update(Duration::fromSeconds(0.2)); // run 0.6 → 0.8, the walk layer 0 → 0.2
    scene.update(Duration::fromSeconds(0.1)); // the walk layer passes 0.25
    CHECK(seen() == "step 0");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep output if a check crashes
    Log::init();
    Log::core().set_level(spdlog::level::off); // the tests trigger expected errors
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"references", references},
        {"scan, meta files and stable GUIDs", scanAndMeta},
        {"importers and loading", importersAndLoading},
        {"scene references by GUID", sceneReferences},
        {"prefabs", prefabs},
        {"models keep their node tree", modelHierarchy},
        {"animation: sampling (linear, step, cubic, slerp)", animationSampling},
        {"animation: a model's clip plays on its nodes (loop, pause, speed, saved by reference)", nodeAnimation},
        {"animation: read from a glTF file", gltfAnimation},
        {"animation: cross-fades, mixing by weight, rotations, in step, by reference", animationMixing},
        {"animation: bone masks (override and averaged, subtrees, rotations)", boneMasks},
        {"animation: events (start, laps, speed, backwards, no loop, fades, layers)", animationEvents},
        {"skinning: joint matrices (bend, the model moving, far out, no skeleton)", skinning},
        {"skinning: a glTF skeleton", gltfSkin},
        {"import", importFiles},
    };
    for (auto& [name, fn] : tests) {
        int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    fs::remove_all(fs::temp_directory_path() / "nova_asset_tests");
    Log::shutdown();
    return g_failures == 0 ? 0 : 1;
}
