#include "nova/scene/SceneSerializer.h"

#include "nova/core/Log.h"
#include "nova/reflect/Reflect.h"
#include "nova/renderer/Renderer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace nova {

using Json = nlohmann::ordered_json; // keeps field order: readable, diff-friendly files

namespace {

constexpr const char* kFormat = "nova.scene";
constexpr const char* kPrefabFormat = "nova.prefab";
constexpr int kVersion = 1;

// ---------------------------------------------------------------- writing --
Json writeObject(const reflect::TypeInfo& type, void* object, const SerializeOptions& opt);

// f32 → the double nearest to its shortest round-trip decimal, so files read "0.55", not
// "0.550000011920929" (reading it back as f32 gives the exact same value).
Json writeFloat(f32 v) {
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    if (ec != std::errc()) return static_cast<double>(v);
    return std::strtod(std::string(buf, end).c_str(), nullptr);
}

// Positions: meters when they convert back exactly (readable, and what older files contain),
// otherwise the exact integer steps with meters alongside for readers.
Json writePosition(const GlobalPosition& g) {
    const glm::dvec3 m = g.toMeters();
    Json meters = Json::array({m.x, m.y, m.z});
    if (GlobalPosition::fromMeters(m) == g) return meters;
    return Json{{"raw", Json::array({g.raw(0), g.raw(1), g.raw(2)})}, {"meters", std::move(meters)}};
}

Json writeOffset(const GlobalOffset& o) {
    const glm::dvec3 m = o.toMeters();
    Json meters = Json::array({m.x, m.y, m.z});
    if (GlobalOffset::fromMeters(m) == o) return meters;
    return Json{{"raw", Json::array({o.x, o.y, o.z})}, {"meters", std::move(meters)}};
}

const char* boundaryName(WorldBoundary b) { return b == WorldBoundary::Wrap ? "Wrap" : "Barrier"; }

Json writeWorld(const WorldSpaceDesc& d) {
    Json axes = Json::array();
    for (const WorldAxisDesc& a : d.axes) axes.push_back(Json{{"size", a.size}, {"boundary", boundaryName(a.boundary)}});
    return Json{{"unit", d.unit}, {"axes", std::move(axes)}};
}

Json writeVec(const f32* v, int n) {
    Json a = Json::array();
    for (int i = 0; i < n; ++i) a.push_back(writeFloat(v[i]));
    return a;
}

Json writeField(const reflect::FieldInfo& f, void* p, const SerializeOptions& opt) {
    using K = reflect::FieldKind;
    switch (f.kind) {
        case K::Bool: return *static_cast<bool*>(p);
        case K::Int: return *static_cast<i32*>(p);
        case K::UInt: return *static_cast<u32*>(p);
        case K::UInt64: return *static_cast<u64*>(p);
        case K::Float: return writeFloat(*static_cast<f32*>(p));
        case K::Vec2: return writeVec(&static_cast<glm::vec2*>(p)->x, 2);
        case K::Vec3: return writeVec(&static_cast<glm::vec3*>(p)->x, 3);
        case K::Vec4: return writeVec(&static_cast<glm::vec4*>(p)->x, 4);
        case K::String: return *static_cast<std::string*>(p);
        case K::Enum: {
            const i64 v = f.getEnum(p);
            const reflect::TypeInfo* e = f.nestedType();
            if (const char* name = e ? e->enumName(v) : nullptr) return name;
            return v;
        }
        case K::Mesh: {
            const MeshHandle m = *static_cast<MeshHandle*>(p);
            if (!m) return nullptr;
            if (opt.meshName) {
                std::string name = opt.meshName(m);
                if (!name.empty()) return name;
            }
            return m.id;
        }
        case K::Texture: {
            const rhi::TextureHandle t = *static_cast<rhi::TextureHandle*>(p);
            if (!t) return nullptr;
            if (opt.textureName) {
                std::string name = opt.textureName(t);
                if (!name.empty()) return name;
            }
            return t.key();
        }
        case K::Position: return writePosition(*static_cast<GlobalPosition*>(p));
        case K::Quat: {
            const glm::quat q = *static_cast<glm::quat*>(p);
            const f32 v[4] = {q.x, q.y, q.z, q.w};
            return writeVec(v, 4);
        }
        case K::Shader: {
            const ShaderHandle sh = *static_cast<ShaderHandle*>(p);
            if (!sh) return nullptr;
            if (opt.shaderName) {
                std::string name = opt.shaderName(sh);
                if (!name.empty()) return name;
            }
            return sh.id;
        }
        case K::Struct: {
            const reflect::TypeInfo* t = f.nestedType();
            if (!t) return nullptr; // unregistered struct
            return writeObject(*t, p, opt);
        }
    }
    return nullptr;
}

Json writeObject(const reflect::TypeInfo& type, void* object, const SerializeOptions& opt) {
    Json j = Json::object();
    for (const auto& f : type.fields) {
        if (f.attrs.transient) continue;
        j[f.name] = writeField(f, f.in(object), opt);
    }
    return j;
}

Json writeEntity(Entity e, const SerializeOptions& opt, std::set<std::string>& warned) {
    Json components = Json::object();
    for (const reflect::TypeInfo* t : reflect::types().components())
        if (t->has(e)) components[t->name] = writeObject(*t, t->get(e), opt);

    Json entity = Json::object();
    entity["components"] = std::move(components);

    if (auto* sc = e.tryGet<ScriptComponent>()) {
        Json scripts = Json::array();
        for (auto& s : sc->scripts) {
            const reflect::TypeInfo* t = reflect::types().find(std::type_index(typeid(*s)));
            if (!t || !t->isScript) {
                if (warned.insert(typeid(*s).name()).second)
                    NOVA_CORE_WARN("Scene: script '{}' is not reflected (reflect::type<T>(...).script()); not saved",
                                   typeid(*s).name());
                continue;
            }
            scripts.push_back(Json{{"type", t->name}, {"fields", writeObject(*t, s.get(), opt)}});
        }
        if (!scripts.empty()) entity["scripts"] = std::move(scripts);
    }
    return entity;
}

// ---------------------------------------------------------------- reading --
void readObject(const reflect::TypeInfo& type, void* object, const Json& j, const SerializeOptions& opt);

bool readRaw(const Json& j, i64 out[3]) {
    if (!j.is_object() || !j.contains("raw") || !j["raw"].is_array() || j["raw"].size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!j["raw"][i].is_number_integer()) return false;
        out[i] = j["raw"][i].get<i64>();
    }
    return true;
}

bool readMeters(const Json& j, glm::dvec3& out) {
    const Json& a = j.is_object() && j.contains("meters") ? j["meters"] : j;
    if (!a.is_array() || a.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!a[i].is_number()) return false;
        out[i] = a[i].get<f64>();
    }
    return true;
}

bool readPosition(const Json& j, GlobalPosition& out) {
    i64 raw[3];
    if (readRaw(j, raw)) {
        out = GlobalPosition::fromRaw(raw[0], raw[1], raw[2]);
        return true;
    }
    glm::dvec3 m;
    if (!readMeters(j, m)) return false;
    out = GlobalPosition::fromMeters(m);
    return true;
}

bool readOffset(const Json& j, GlobalOffset& out) {
    i64 raw[3];
    if (readRaw(j, raw)) {
        out = {raw[0], raw[1], raw[2]};
        return true;
    }
    glm::dvec3 m;
    if (!readMeters(j, m)) return false;
    out = GlobalOffset::fromMeters(m);
    return true;
}

WorldSpaceDesc readWorld(const Json& j) {
    WorldSpaceDesc d;
    if (!j.is_object()) return d;
    if (j.contains("unit") && j["unit"].is_number()) d.unit = j["unit"].get<f64>();
    if (j.contains("axes") && j["axes"].is_array())
        for (int i = 0; i < 3 && i < static_cast<int>(j["axes"].size()); ++i) {
            const Json& a = j["axes"][i];
            if (!a.is_object()) continue;
            if (a.contains("size") && a["size"].is_number()) d.axes[i].size = a["size"].get<f64>();
            d.axes[i].boundary = a.value("boundary", "Barrier") == "Wrap" ? WorldBoundary::Wrap : WorldBoundary::Barrier;
        }
    return d;
}

template <int N>
void readVec(f32* out, const Json& j) {
    if (!j.is_array()) return;
    for (int i = 0; i < N && i < static_cast<int>(j.size()); ++i)
        if (j[i].is_number()) out[i] = j[i].get<f32>();
}

void readField(const reflect::FieldInfo& f, void* p, const Json& j, const SerializeOptions& opt) {
    using K = reflect::FieldKind;
    switch (f.kind) {
        case K::Bool: if (j.is_boolean()) *static_cast<bool*>(p) = j.get<bool>(); break;
        case K::Int: if (j.is_number()) *static_cast<i32*>(p) = j.get<i32>(); break;
        case K::UInt: if (j.is_number()) *static_cast<u32*>(p) = j.get<u32>(); break;
        case K::UInt64: if (j.is_number()) *static_cast<u64*>(p) = j.get<u64>(); break;
        case K::Float: if (j.is_number()) *static_cast<f32*>(p) = j.get<f32>(); break;
        case K::Vec2: readVec<2>(&static_cast<glm::vec2*>(p)->x, j); break;
        case K::Vec3: readVec<3>(&static_cast<glm::vec3*>(p)->x, j); break;
        case K::Vec4: readVec<4>(&static_cast<glm::vec4*>(p)->x, j); break;
        case K::String: if (j.is_string()) *static_cast<std::string*>(p) = j.get<std::string>(); break;
        case K::Enum: {
            if (j.is_number_integer()) f.setEnum(p, j.get<i64>());
            else if (j.is_string()) {
                const reflect::TypeInfo* e = f.nestedType();
                if (auto v = e ? e->enumValue(j.get<std::string>()) : std::nullopt) f.setEnum(p, *v);
                else NOVA_CORE_WARN("Scene: unknown value '{}' for '{}'", j.get<std::string>(), f.name);
            }
            break;
        }
        case K::Mesh: {
            auto& m = *static_cast<MeshHandle*>(p);
            if (j.is_null()) m = {};
            else if (j.is_number_integer()) m = {j.get<u32>()};
            else if (j.is_string()) {
                const std::string name = j.get<std::string>();
                m = opt.findMesh ? opt.findMesh(name) : MeshHandle{};
                if (!m) NOVA_CORE_WARN("Scene: mesh '{}' not found", name);
            }
            break;
        }
        case K::Texture: {
            auto& t = *static_cast<rhi::TextureHandle*>(p);
            if (j.is_null()) t = {};
            else if (j.is_number_unsigned() || j.is_number_integer()) {
                const u64 key = j.get<u64>();
                t = rhi::TextureHandle{static_cast<u32>(key & 0xFFFFFFFFu), static_cast<u32>(key >> 32)};
            } else if (j.is_string()) {
                const std::string name = j.get<std::string>();
                t = opt.findTexture ? opt.findTexture(name) : rhi::TextureHandle{};
                if (!t) NOVA_CORE_WARN("Scene: texture '{}' not found", name);
            }
            break;
        }
        case K::Position: readPosition(j, *static_cast<GlobalPosition*>(p)); break;
        case K::Quat: {
            auto& q = *static_cast<glm::quat*>(p);
            if (!j.is_array()) break;
            if (j.size() == 4) { // [x, y, z, w]
                f32 v[4] = {0, 0, 0, 1};
                readVec<4>(v, j);
                q = glm::normalize(glm::quat(v[3], v[0], v[1], v[2]));
            } else if (j.size() == 3) { // older files: Euler radians in glm::quat(vec3) convention (ZYX)
                glm::vec3 e(0.0f);
                readVec<3>(&e.x, j);
                q = quatFromEuler(e, EulerOrder::ZYX);
            }
            break;
        }
        case K::Shader: {
            auto& sh = *static_cast<ShaderHandle*>(p);
            if (j.is_null()) sh = {};
            else if (j.is_number_integer()) sh = {j.get<u32>()};
            else if (j.is_string()) {
                const std::string name = j.get<std::string>();
                sh = opt.findShader ? opt.findShader(name) : ShaderHandle{};
                if (!sh) NOVA_CORE_WARN("Scene: shader '{}' not found", name);
            }
            break;
        }
        case K::Struct:
            if (const reflect::TypeInfo* t = f.nestedType(); t && j.is_object()) readObject(*t, p, j, opt);
            break;
    }
}

void readObject(const reflect::TypeInfo& type, void* object, const Json& j, const SerializeOptions& opt) {
    if (!j.is_object()) return;
    for (const auto& f : type.fields) {
        if (f.attrs.transient) continue;
        auto it = j.find(f.name);
        if (it != j.end()) readField(f, f.in(object), *it, opt); // missing = keep default
    }
}

void readEntityInto(Entity e, const Json& je, const SerializeOptions& opt, std::set<std::string>& warned);

Entity readEntity(Scene& scene, const Json& je, const SerializeOptions& opt, std::set<std::string>& warned) {
    Entity e = scene.createEntity();
    readEntityInto(e, je, opt, warned);
    return e;
}

void readEntityInto(Entity e, const Json& je, const SerializeOptions& opt, std::set<std::string>& warned) {
    const Json& components = je.contains("components") ? je["components"] : Json::object();
    for (auto it = components.begin(); it != components.end(); ++it) {
        const reflect::TypeInfo* t = reflect::types().find(std::string_view(it.key()));
        if (!t || !t->isComponent) {
            if (warned.insert(it.key()).second) NOVA_CORE_WARN("Scene: unknown component '{}' skipped", it.key());
            continue;
        }
        if (!t->has(e)) t->add(e);
        readObject(*t, t->get(e), it.value(), opt);
    }
    if (je.contains("scripts") && je["scripts"].is_array()) {
        for (const Json& js : je["scripts"]) {
            const std::string typeName = js.value("type", "");
            const reflect::TypeInfo* t = reflect::types().find(std::string_view(typeName));
            if (!t || !t->isScript) {
                if (warned.insert(typeName).second) NOVA_CORE_WARN("Scene: unknown script '{}' skipped", typeName);
                continue;
            }
            Scope<Script> script = t->createScript();
            if (js.contains("fields")) readObject(*t, script.get(), js["fields"], opt);
            e.attachScript(std::move(script));
        }
    }
}

// Entities in creation order (EnTT views iterate newest first).
std::vector<entt::entity> entitiesInOrder(Scene& scene) {
    std::vector<entt::entity> out;
    for (auto e : scene.registry().view<IdComponent>()) out.push_back(e);
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace

SerializeOptions SerializeOptions::fromRenderer(Renderer& renderer) {
    SerializeOptions o;
    o.meshName = [&renderer](MeshHandle m) { return renderer.meshName(m); };
    o.findMesh = [&renderer](const std::string& name) { return renderer.findMesh(name); };
    return o;
}

std::string serializeScene(Scene& scene, const SerializeOptions& options) {
    std::set<std::string> warned;
    Json root = Json::object();
    root["format"] = kFormat;
    root["version"] = kVersion;
    root["name"] = scene.name();
    root["world"] = writeWorld(scene.worldSpace());
    Json entities = Json::array();
    for (entt::entity e : entitiesInOrder(scene)) entities.push_back(writeEntity({e, &scene}, options, warned));
    root["entities"] = std::move(entities);
    return root.dump(2);
}

bool deserializeScene(Scene& scene, std::string_view text, const SerializeOptions& options, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    Json root = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) return fail("invalid JSON");
    if (!root.is_object() || root.value("format", "") != kFormat) return fail("not a nova.scene file");
    if (root.value("version", 0) > kVersion) return fail("scene file is from a newer engine version");

    scene.clear();
    if (root.contains("name") && root["name"].is_string()) scene.setName(root["name"].get<std::string>());
    // The world settings come first: positions are stored in its units. Files without them
    // (older versions) use the default world, where their meter positions mean the same.
    scene.setWorldSpace(root.contains("world") ? readWorld(root["world"]) : WorldSpaceDesc{});
    std::set<std::string> warned;
    try {
        if (root.contains("entities") && root["entities"].is_array())
            for (const Json& je : root["entities"]) readEntity(scene, je, options, warned);
    } catch (const nlohmann::json::exception& ex) {
        scene.clear();
        return fail(std::string("malformed scene: ") + ex.what());
    }
    return true;
}

bool saveScene(Scene& scene, const std::filesystem::path& path, const SerializeOptions& options, std::string* error) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    file << serializeScene(scene, options) << '\n';
    return static_cast<bool>(file);
}

bool loadScene(Scene& scene, const std::filesystem::path& path, const SerializeOptions& options, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    if (!deserializeScene(scene, ss.str(), options, error)) return false;
    scene.setName(sceneNameFromPath(path));
    return true;
}

std::string sceneNameFromPath(const std::filesystem::path& path) {
    std::string name = path.filename().string();
    for (const char* suffix : {".json", ".scene"})
        if (name.size() > std::strlen(suffix) && name.ends_with(suffix)) name.resize(name.size() - std::strlen(suffix));
    return name;
}

Entity duplicateEntity(Entity entity) {
    Scene& scene = *entity.scene();
    std::set<std::string> warned;
    SerializeOptions ids; // same session: exact mesh ids
    Json je = writeEntity(entity, ids, warned);
    je["components"].erase("Id"); // the copy keeps its own new UUID
    Entity copy = readEntity(scene, je, ids, warned);
    if (auto* name = copy.tryGet<NameComponent>()) name->name += " (copy)";
    return copy;
}

std::string serializePrefab(const std::vector<Entity>& entities, const SerializeOptions& options) {
    std::set<std::string> warned;
    Json root = Json::object();
    root["format"] = kPrefabFormat;
    root["version"] = kVersion;
    Json list = Json::array();
    GlobalPosition pivot;
    bool first = true;
    for (Entity e : entities) {
        if (!e.valid()) continue;
        if (first) pivot = e.transform().position;
        first = false;
        Json je = writeEntity(e, options, warned);
        je["components"].erase("Id");
        je["components"].erase("PrefabInstance");
        // Relative to the first entity, so the prefab can be placed anywhere.
        je["components"]["Transform"]["position"] = writeOffset(e.transform().position - pivot);
        list.push_back(std::move(je));
    }
    root["entities"] = std::move(list);
    return root.dump(2);
}

std::vector<Entity> instantiatePrefab(Scene& scene, std::string_view json, const GlobalPosition& position,
                                      const SerializeOptions& options, std::string* error) {
    std::vector<Entity> out;
    Json root = Json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object() || root.value("format", "") != kPrefabFormat) {
        if (error) *error = "not a nova.prefab file";
        return out;
    }
    std::set<std::string> warned;
    try {
        if (root.contains("entities") && root["entities"].is_array())
            for (Json je : root["entities"]) {
                GlobalOffset local;
                if (je.contains("components")) {
                    je["components"].erase("Id"); // fresh UUIDs
                    if (je["components"].contains("Transform") && je["components"]["Transform"].contains("position")) {
                        readOffset(je["components"]["Transform"]["position"], local);
                        je["components"]["Transform"].erase("position");
                    }
                }
                Entity e = readEntity(scene, je, options, warned);
                e.transform().position = position + local;
                out.push_back(e);
            }
    } catch (const nlohmann::json::exception& ex) {
        if (error) *error = std::string("malformed prefab: ") + ex.what();
    }
    return out;
}

std::string serializeEntity(Entity entity, const SerializeOptions& options) {
    std::set<std::string> warned;
    return writeEntity(entity, options, warned).dump();
}

Entity restoreEntity(Scene& scene, std::string_view json, const SerializeOptions& options) {
    Json je = Json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (je.is_discarded() || !je.is_object()) return {};
    std::set<std::string> warned;

    u64 uuid = 0;
    if (je.contains("components") && je["components"].contains("Id"))
        uuid = je["components"]["Id"].value("uuid", u64{0});
    Entity e = scene.findEntityByUuid(uuid);
    if (!e.valid()) return readEntity(scene, je, options, warned);

    // In place: drop reflected state (and scripts), then apply the JSON.
    for (const reflect::TypeInfo* t : reflect::types().components())
        if (t->name != "Id" && t->has(e)) t->remove(e);
    if (e.has<ScriptComponent>()) e.remove<ScriptComponent>();
    readEntityInto(e, je, options, warned);
    if (!e.has<NameComponent>()) e.add<NameComponent>();
    if (!e.has<TransformComponent>()) e.add<TransformComponent>();
    return e;
}

} // namespace nova
