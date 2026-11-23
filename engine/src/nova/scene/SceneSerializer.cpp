#include "nova/scene/SceneSerializer.h"

#include "nova/core/Log.h"
#include "nova/reflect/Reflect.h"
#include "nova/renderer/Renderer.h"

#include <functional>
#include <unordered_map>
#include <typeindex>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
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

// Positions and offsets in meters: a step count times 2^-18 is exact in a double below 2^53 steps
// (3.4×10¹⁰ m), so meters convert back to the same bits. Beyond that (only in huge worlds) the
// exact steps go along: {"steps": [...], "meters": [...]}.
constexpr i64 kExactMeterSteps = i64(1) << 53;
Json writeSteps(const glm::i64vec3& s, const glm::dvec3& m) {
    auto exact = [](i64 v) { return v > -kExactMeterSteps && v < kExactMeterSteps; };
    if (exact(s.x) && exact(s.y) && exact(s.z)) return Json::array({m.x, m.y, m.z});
    return Json{{"steps", Json::array({s.x, s.y, s.z})}, {"meters", Json::array({m.x, m.y, m.z})}};
}

Json writePosition(const GlobalPosition& g) { return writeSteps(g.raw(), g.toMeters()); }

Json writeOffset(const GlobalOffset& o) { return writeSteps(o.raw(), o.toMeters()); }

// A fixed-point value as the shortest decimal that reads back to the same raw integer ("0.05",
// not "0.0500030517578125"): files stay readable and round trips are exact.
Json writeFixed(i32 raw, u32 fraction) {
    const f64 exact = fromRawValue(raw, fraction);
    char buf[40];
    for (int digits = 1; digits <= 17; ++digits) {
        std::snprintf(buf, sizeof(buf), "%.*g", digits, exact);
        const f64 v = std::strtod(buf, nullptr);
        if (toRaw(v, fraction) == raw) return v;
    }
    return exact;
}

Json writeFixedVec(const reflect::FieldInfo& f, const void* p) {
    Json a = Json::array();
    for (int i = 0; i < 3; ++i) a.push_back(writeFixed(reflect::fixedRaw(f, p, i), f.fracBits));
    return a;
}

Json writeFixedQuat(const FixedQuat& q) {
    constexpr u32 kBits = fractionBits<Unit>;
    return Json::array({writeFixed(q.x.raw_value(), kBits), writeFixed(q.y.raw_value(), kBits),
                        writeFixed(q.z.raw_value(), kBits), writeFixed(q.w.raw_value(), kBits)});
}

Json writeSpeedVec(const SpeedVec& v) {
    constexpr u32 kBits = fractionBits<Speed>;
    return Json::array({writeFixed(v.x.raw_value(), kBits), writeFixed(v.y.raw_value(), kBits),
                        writeFixed(v.z.raw_value(), kBits)});
}

// The world block: the step (informative) and the size of each axis (all circular).
Json writeWorld(const WorldSize& w) {
    Json axes = Json::array();
    for (int i = 0; i < 3; ++i) axes.push_back(Json{{"size", w.size(i)}});
    return Json{{"step", WorldSpace::kUnit}, {"axes", std::move(axes)}};
}

Json writeVec(const f32* v, int n) {
    Json a = Json::array();
    for (int i = 0; i < n; ++i) a.push_back(writeFloat(v[i]));
    return a;
}

// Physics settings: gravity, named layers and (when not the default) the collision matrix.
Json writePhysics(const PhysicsSettings& s) {
    Json j = Json::object();
    j["gravity"] = writeSpeedVec(s.gravity);
    Json names = Json::object();
    for (u32 i = 0; i < physics::kCollisionLayerCount; ++i)
        if (!s.layerNames[i].empty()) names[std::to_string(i)] = s.layerNames[i];
    if (!names.empty()) j["layerNames"] = std::move(names);
    if (!(s.layers == physics::CollisionLayers{})) {
        Json m = Json::array();
        for (u32 mask : s.layers.mask) m.push_back(mask);
        j["collisionMatrix"] = std::move(m);
    }
    return j;
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
        case K::Entity: return static_cast<EntityRef*>(p)->uuid;
        case K::Fixed: return writeFixed(reflect::fixedRaw(f, p), f.fracBits);
        case K::FixedVec3: return writeFixedVec(f, p);
        case K::FixedQuat: return writeFixedQuat(*static_cast<FixedQuat*>(p));
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

bool readMeters(const Json& j, glm::dvec3& out) {
    const Json& a = j.is_object() && j.contains("meters") ? j["meters"] : j;
    if (!a.is_array() || a.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!a[i].is_number()) return false;
        out[i] = a[i].get<f64>();
    }
    return true;
}

// Exact steps, when the file has them ({"steps": [...]}, huge worlds).
bool readSteps(const Json& j, glm::i64vec3& out) {
    if (!j.is_object() || !j.contains("steps")) return false;
    const Json& a = j["steps"];
    if (!a.is_array() || a.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!a[i].is_number_integer()) return false;
        out[i] = a[i].get<i64>();
    }
    return true;
}

// Meters (older files may also carry "raw" steps of another unit: the meters next to them win).
bool readPosition(const Json& j, GlobalPosition& out) {
    if (glm::i64vec3 s; readSteps(j, s)) {
        out = GlobalPosition::fromRaw(s);
        return true;
    }
    glm::dvec3 m;
    if (!readMeters(j, m)) return false;
    out = GlobalPosition::fromMeters(m);
    return true;
}

bool readOffset(const Json& j, GlobalOffset& out) {
    if (glm::i64vec3 s; readSteps(j, s)) {
        out = GlobalOffset() + GlobalOffset(s); // into the world
        return true;
    }
    glm::dvec3 m;
    if (!readMeters(j, m)) return false;
    out = GlobalOffset::fromMeters(m);
    return true;
}

template <int N>
void readVec(f32* out, const Json& j) {
    if (!j.is_array()) return;
    for (int i = 0; i < N && i < static_cast<int>(j.size()); ++i)
        if (j[i].is_number()) out[i] = j[i].get<f32>();
}

// Numbers → the nearest fixed-point values (exact for what writeFixed wrote).
void readFixedVec(const reflect::FieldInfo& f, void* p, const Json& j) {
    if (!j.is_array()) return;
    for (int i = 0; i < 3 && i < static_cast<int>(j.size()); ++i)
        if (j[i].is_number()) reflect::setFixedValue(f, p, j[i].get<f64>(), i);
}

void readSpeedVec(SpeedVec& v, const Json& j) {
    if (!j.is_array()) return;
    for (int i = 0; i < 3 && i < static_cast<int>(j.size()); ++i)
        if (j[i].is_number()) v[i] = toFixed<Speed>(j[i].get<f64>());
}

// [x, y, z, w]: kept bit for bit when already unit (what writeFixedQuat wrote), normalized
// otherwise; older files with 3 numbers hold Euler radians (glm::quat(vec3) convention, ZYX).
void readFixedQuat(FixedQuat& q, const Json& j) {
    if (!j.is_array()) return;
    if (j.size() == 4) {
        f64 v[4] = {0, 0, 0, 1};
        for (int i = 0; i < 4; ++i)
            if (j[i].is_number()) v[i] = j[i].get<f64>();
        FixedQuat r;
        r.x = toFixed<Unit>(v[0]);
        r.y = toFixed<Unit>(v[1]);
        r.z = toFixed<Unit>(v[2]);
        r.w = toFixed<Unit>(v[3]);
        const f64 len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
        q = std::abs(len2 - 1.0) < 1e-6 ? r : FixedQuat(glm::dquat(v[3], v[0], v[1], v[2]));
    } else if (j.size() == 3) {
        glm::vec3 e(0.0f);
        readVec<3>(&e.x, j);
        q = quatFromEuler(e, EulerOrder::ZYX);
    }
}

// Axes of the world block. Every axis is circular: older files with barrier edges load as
// circular worlds of the same size (with a warning); files of the int64 format (unit, boundary)
// load the default world.
WorldSize readWorld(const Json& j) {
    WorldSize w;
    if (!j.is_object() || !j.contains("axes") || !j["axes"].is_array()) return w;
    for (size_t i = 0; i < 3 && i < j["axes"].size(); ++i) {
        const Json& a = j["axes"][i];
        if (!a.is_object() || !a.contains("size") || !a["size"].is_number()) continue;
        if (a.contains("boundary") || j.contains("unit")) continue; // the int64 format: the default world
        if (a.value("edge", "Wrap") == "Barrier")
            NOVA_CORE_WARN("Scene: axis {} had barrier edges; the world is always circular now", i);
        w.steps[static_cast<int>(i)] = WorldSize::toSteps(a["size"].get<f64>());
    }
    return w;
}

PhysicsSettings readPhysics(const Json& j) {
    PhysicsSettings s;
    if (!j.is_object()) return s;
    if (j.contains("gravity")) readSpeedVec(s.gravity, j["gravity"]);
    if (j.contains("layerNames") && j["layerNames"].is_object())
        for (auto it = j["layerNames"].begin(); it != j["layerNames"].end(); ++it) {
            const int i = std::atoi(it.key().c_str());
            if (i >= 0 && i < static_cast<int>(physics::kCollisionLayerCount) && it.value().is_string())
                s.layerNames[static_cast<size_t>(i)] = it.value().get<std::string>();
        }
    if (j.contains("collisionMatrix") && j["collisionMatrix"].is_array())
        for (u32 i = 0; i < physics::kCollisionLayerCount && i < j["collisionMatrix"].size(); ++i)
            if (j["collisionMatrix"][i].is_number_unsigned()) s.layers.mask[i] = j["collisionMatrix"][i].get<u32>();
    return s;
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
        case K::Entity:
            if (j.is_number_unsigned() || j.is_number_integer()) static_cast<EntityRef*>(p)->uuid = j.get<u64>();
            else if (j.is_null()) *static_cast<EntityRef*>(p) = {};
            break;
        case K::Fixed: if (j.is_number()) reflect::setFixedValue(f, p, j.get<f64>()); break;
        case K::FixedVec3: readFixedVec(f, p, j); break;
        case K::FixedQuat: readFixedQuat(*static_cast<FixedQuat*>(p), j); break;
        case K::Struct:
            if (const reflect::TypeInfo* t = f.nestedType(); t && j.is_object()) readObject(*t, p, j, opt);
            break;
    }
}

// Every EntityRef inside a reflected object (nested structs included).
void forEachEntityRef(const reflect::TypeInfo& type, void* object, const std::function<void(EntityRef&)>& fn) {
    for (const auto& f : type.fields) {
        if (f.kind == reflect::FieldKind::Entity) fn(*static_cast<EntityRef*>(f.in(object)));
        else if (f.kind == reflect::FieldKind::Struct)
            if (const reflect::TypeInfo* t = f.nestedType()) forEachEntityRef(*t, f.in(object), fn);
    }
}

// References between entities of a copy point at the copies (others stay as they are).
void remapEntityRefs(Entity e, const std::unordered_map<u64, u64>& map) {
    auto remap = [&](EntityRef& r) {
        if (auto it = map.find(r.uuid); it != map.end()) r.uuid = it->second;
    };
    for (const reflect::TypeInfo* t : reflect::types().components())
        if (t->name != "Id" && t->has(e)) forEachEntityRef(*t, t->get(e), remap);
    if (auto* sc = e.tryGet<ScriptComponent>())
        for (auto& s : sc->scripts)
            if (const reflect::TypeInfo* t = reflect::types().find(std::type_index(typeid(*s)))) forEachEntityRef(*t, s.get(), remap);
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
    root["world"] = writeWorld(scene.worldSize());
    root["physics"] = writePhysics(scene.physicsSettings());
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
    scene.setPhysicsSettings(root.contains("physics") ? readPhysics(root["physics"]) : PhysicsSettings{});
    // The world first: positions land in it.
    scene.setWorldSize(root.contains("world") ? readWorld(root["world"]) : WorldSize{});
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
        je["prefabId"] = e.get<IdComponent>().uuid; // to remap references between the prefab's entities
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
    std::unordered_map<u64, u64> ids; // prefab id -> new UUID
    try {
        if (root.contains("entities") && root["entities"].is_array())
            for (Json je : root["entities"]) {
                const u64 prefabId = je.value("prefabId", u64{0});
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
                if (prefabId) ids[prefabId] = e.get<IdComponent>().uuid;
                out.push_back(e);
            }
    } catch (const nlohmann::json::exception& ex) {
        if (error) *error = std::string("malformed prefab: ") + ex.what();
    }
    for (Entity e : out) remapEntityRefs(e, ids);
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
