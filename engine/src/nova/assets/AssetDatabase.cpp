#include "nova/assets/AssetDatabase.h"

#include "nova/assets/GltfLoader.h"
#include "nova/core/Log.h"
#include "nova/renderer/Renderer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

namespace nova {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kRefPrefix = "asset:";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string toHex(AssetGuid guid) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(guid));
    return buf;
}

bool fromHex(std::string_view text, AssetGuid& out) {
    if (text.empty() || text.size() > 16) return false;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out, 16);
    return ec == std::errc() && ptr == text.data() + text.size();
}

bool readText(const fs::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    return true;
}

bool isInside(const fs::path& child, const fs::path& parent) {
    const fs::path rel = child.lexically_normal().lexically_relative(parent.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

} // namespace

AssetDatabase::AssetDatabase(Renderer* renderer) : m_renderer(renderer) { registerBuiltinImporters(); }

AssetDatabase::~AssetDatabase() = default;

// --------------------------------------------------------------- importers --
void AssetDatabase::registerBuiltinImporters() {
    addImporter({
        .type = "Texture",
        .extensions = {".png", ".jpg", ".jpeg", ".tga", ".bmp"},
        .load = [](AssetDatabase& db, const fs::path& file, AssetData& out) {
            if (!db.renderer()) return false;
            u32 w = 0, h = 0;
            std::vector<u8> pixels = decodeImageFile(file, w, h);
            if (pixels.empty()) return false;
            out.textures.push_back(db.renderer()->createTexture2D(w, h, pixels.data(), file.filename().string().c_str()));
            return true;
        },
    });

    addImporter({
        .type = "Model",
        .extensions = {".gltf", ".glb"},
        .load = [](AssetDatabase& db, const fs::path& file, AssetData& out) {
            if (!db.renderer()) return false;
            auto model = loadModel(*db.renderer(), file);
            if (!model) return false;
            out.meshes = model->meshes;
            out.textures = model->textures;
            out.payload = std::make_shared<ModelData>(std::move(*model));
            return true;
        },
        .instantiate = [](AssetDatabase&, const AssetData& data, Scene& scene, const GlobalPosition& position) {
            return instantiateModel(scene, *data.as<ModelData>(), position);
        },
        .dependencies = gltfDependencies,
    });

    addImporter({
        .type = "Shader",
        .extensions = {".vert", ".frag", ".comp", ".vert.glsl", ".frag.glsl", ".comp.glsl", ".vert.spv", ".frag.spv",
                       ".comp.spv"},
        .load = [](AssetDatabase& db, const fs::path& file, AssetData& out) {
            if (!db.renderer()) return false;
            const ShaderHandle h = db.renderer()->shaders().load(file);
            if (!h) return false;
            out.shaders.push_back(h);
            return true;
        },
    });

    addImporter({
        .type = "Prefab",
        .extensions = {".prefab.json"},
        .load = [](AssetDatabase&, const fs::path& file, AssetData& out) {
            auto text = std::make_shared<std::string>();
            if (!readText(file, *text)) return false;
            out.payload = std::move(text);
            return true;
        },
        .instantiate = [](AssetDatabase& db, const AssetData& data, Scene& scene, const GlobalPosition& position) {
            std::string error;
            auto entities = instantiatePrefab(scene, *data.as<std::string>(), position, db.serializeOptions(), &error);
            if (!error.empty()) NOVA_CORE_ERROR("Prefab: {}", error);
            return entities;
        },
    });

    addImporter({.type = "Scene", .extensions = {".scene.json"}});
}

void AssetDatabase::addImporter(AssetImporter importer) {
    for (auto& e : importer.extensions) e = lower(e);
    importer.module = currentModule();
    m_importers.push_back(std::move(importer));
    refresh(); // re-resolve importers of known files (cheap; no-op without a root)
}

void AssetDatabase::removeModule(ModuleId module) {
    if (module == 0) return;
    // Loaded data may hold payloads whose destructors live in the module: drop it first.
    for (const AssetRecord& r : m_records)
        if (r.importer && r.importer->module == module) unload(r.guid);
    const size_t before = m_importers.size();
    std::erase_if(m_importers, [module](const AssetImporter& i) { return i.module == module; });
    if (m_importers.size() != before) refresh(); // records point at importers
}

const AssetImporter* AssetDatabase::importerFor(const fs::path& file) const {
    const std::string name = lower(file.filename().string());
    const AssetImporter* best = nullptr;
    size_t bestLength = 0;
    for (const AssetImporter& imp : m_importers) // later importers win ties (>=)
        for (const std::string& ext : imp.extensions)
            if (name.size() > ext.size() && name.ends_with(ext) && ext.size() >= bestLength) {
                best = &imp;
                bestLength = ext.size();
            }
    return best;
}

// ------------------------------------------------------------------- files --
void AssetDatabase::setRoot(const fs::path& root) {
    std::error_code ec;
    fs::create_directories(root, ec);
    m_root = fs::weakly_canonical(fs::absolute(root), ec);
    refresh();
}

AssetGuid AssetDatabase::newGuid() {
    static std::mt19937_64 rng{std::random_device{}() ^ (static_cast<u64>(std::random_device{}()) << 32)};
    AssetGuid g = 0;
    while (g == 0) g = rng();
    return g;
}

AssetGuid AssetDatabase::readOrCreateMeta(const fs::path& file, const AssetImporter& importer) {
    fs::path meta = file;
    meta += ".meta";
    std::string text;
    if (readText(meta, text)) {
        auto j = nlohmann::json::parse(text, nullptr, false);
        AssetGuid g = 0;
        if (j.is_object() && j.contains("guid") && j["guid"].is_string() && fromHex(j["guid"].get<std::string>(), g) && g)
            return g;
        NOVA_CORE_WARN("Assets: invalid meta file '{}', regenerating", meta.string());
    }
    const AssetGuid g = newGuid();
    nlohmann::ordered_json j;
    j["guid"] = toHex(g);
    j["type"] = importer.type;
    std::ofstream out(meta, std::ios::binary);
    out << j.dump(2) << '\n';
    return g;
}

void AssetDatabase::refresh() {
    m_records.clear();
    if (m_root.empty() || !fs::is_directory(m_root)) return;

    std::unordered_set<AssetGuid> seen;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        const fs::path& file = it->path();
        if (file.extension() == ".meta") continue;
        const AssetImporter* importer = importerFor(file);
        if (!importer) continue;

        AssetGuid guid = readOrCreateMeta(file, *importer);
        if (!seen.insert(guid).second) { // a copied file kept its .meta: give the copy a new identity
            fs::path meta = file;
            meta += ".meta";
            fs::remove(meta, ec);
            guid = readOrCreateMeta(file, *importer);
            seen.insert(guid);
            NOVA_CORE_WARN("Assets: duplicate GUID for '{}', assigned a new one", file.string());
        }
        AssetRecord r;
        r.guid = guid;
        r.path = fs::path(file.lexically_relative(m_root).generic_string());
        r.type = importer->type;
        r.importer = importer;
        m_records.push_back(std::move(r));
    }
    std::sort(m_records.begin(), m_records.end(),
              [](const AssetRecord& a, const AssetRecord& b) { return a.path.generic_string() < b.path.generic_string(); });
    m_failed.clear();
}

const AssetRecord* AssetDatabase::find(AssetGuid guid) const {
    for (const AssetRecord& r : m_records)
        if (r.guid == guid) return &r;
    return nullptr;
}

AssetRecord* AssetDatabase::findMutable(AssetGuid guid) { return const_cast<AssetRecord*>(find(guid)); }

fs::path AssetDatabase::relative(const fs::path& path) const {
    if (!path.is_absolute()) return fs::path(path.lexically_normal().generic_string());
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    return fs::path(canonical.lexically_relative(m_root).generic_string());
}

const AssetRecord* AssetDatabase::findByPath(const fs::path& path) const {
    const std::string rel = relative(path).generic_string();
    for (const AssetRecord& r : m_records)
        if (r.path.generic_string() == rel) return &r;
    return nullptr;
}

std::optional<AssetGuid> AssetDatabase::import(const fs::path& file, const fs::path& folder, std::string* error) {
    auto fail = [&](std::string msg) -> std::optional<AssetGuid> {
        if (error) *error = std::move(msg);
        return std::nullopt;
    };
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return fail("file not found: " + file.string());
    const AssetImporter* importer = importerFor(file);
    if (!importer) return fail("no importer for '" + file.filename().string() + "'");
    if (m_root.empty()) return fail("the asset database has no root folder");

    const fs::path source = fs::weakly_canonical(fs::absolute(file), ec);
    fs::path destination = source;
    if (!isInside(source, m_root)) {
        const fs::path dir = m_root / folder;
        fs::create_directories(dir, ec);
        destination = dir / source.filename();
        if (!fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec))
            return fail("cannot copy to " + destination.string() + ": " + ec.message());
        if (importer->dependencies)
            for (const fs::path& dep : importer->dependencies(source)) {
                fs::path rel = dep.lexically_normal().lexically_relative(source.parent_path());
                if (rel.empty() || *rel.begin() == "..") rel = dep.filename();
                fs::create_directories((dir / rel).parent_path(), ec);
                if (!fs::copy_file(dep, dir / rel, fs::copy_options::overwrite_existing, ec))
                    NOVA_CORE_WARN("Assets: could not copy dependency '{}': {}", dep.string(), ec.message());
            }
    }
    refresh();
    const AssetRecord* r = findByPath(destination);
    if (!r) return fail("import failed");
    NOVA_CORE_INFO("Assets: imported '{}' as {}", r->path.generic_string(), r->type);
    return r->guid;
}

// ----------------------------------------------------------------- loading --
const AssetData* AssetDatabase::load(AssetGuid guid) {
    if (auto it = m_loaded.find(guid); it != m_loaded.end()) return &it->second;
    if (m_failed.contains(guid)) return nullptr;
    const AssetRecord* r = find(guid);
    if (!r || !r->importer || !r->importer->load) return nullptr;

    AssetData data;
    if (!r->importer->load(*this, absolutePath(*r), data)) {
        NOVA_CORE_ERROR("Assets: failed to load '{}'{}", r->path.generic_string(), m_renderer ? "" : " (no renderer)");
        m_failed.insert(guid);
        return nullptr;
    }
    for (u32 i = 0; i < data.meshes.size(); ++i)
        if (data.meshes[i]) m_meshOwners[data.meshes[i].id] = {guid, i};
    for (u32 i = 0; i < data.textures.size(); ++i)
        if (data.textures[i]) m_textureOwners[data.textures[i].key()] = {guid, i};
    for (u32 i = 0; i < data.shaders.size(); ++i)
        if (data.shaders[i]) m_shaderOwners[data.shaders[i].id] = {guid, i};
    return &m_loaded.emplace(guid, std::move(data)).first->second;
}

void AssetDatabase::unload(AssetGuid guid) {
    m_loaded.erase(guid);
    m_failed.erase(guid);
    // Owner maps are kept: entities may still use the old resources and must keep saving them by GUID.
}

std::vector<Entity> AssetDatabase::instantiate(AssetGuid guid, Scene& scene, const GlobalPosition& position) {
    const AssetRecord* r = find(guid);
    if (!r || !canInstantiate(*r)) return {};
    const AssetData* data = load(guid);
    if (!data) return {};
    std::vector<Entity> entities = r->importer->instantiate(*this, *data, scene, position);
    for (Entity e : entities) {
        if (!e.valid()) continue;
        if (!e.has<PrefabInstanceComponent>()) e.add<PrefabInstanceComponent>();
        e.get<PrefabInstanceComponent>().asset = guid;
    }
    if (!entities.empty() && r->type == "Model") {
        entities[0].get<NameComponent>().name = r->path.stem().string(); // the root
        if (auto* anim = entities[0].tryGet<AnimatorComponent>()) { // its clip, by reference: saved scenes find it again
            anim->clip = makeRef(guid, 0);
            anim->resolved = anim->clip;
        }
        if (const ModelData* model = data->as<ModelData>()) // the skeletons too
            for (Entity e : entities)
                if (auto* sc = e.tryGet<SkinComponent>())
                    for (u32 k = 0; k < model->skins.size(); ++k)
                        if (model->skins[k] == sc->data) {
                            sc->skin = makeRef(guid, k);
                            sc->resolved = sc->skin;
                        }
    }
    return entities;
}

Scene::AnimationSource AssetDatabase::animationSource() {
    return [this](const std::string& ref) -> std::shared_ptr<const AnimationClip> {
        AssetGuid guid = 0;
        u32 index = 0;
        if (!parseRef(ref, guid, index)) return nullptr;
        const AssetData* data = load(guid);
        const ModelData* model = data ? data->as<ModelData>() : nullptr;
        const AssetRecord* r = find(guid);
        if (!model || !r || r->type != "Model" || index >= model->animations.size()) return nullptr;
        return model->animations[index];
    };
}

Scene::SkinSource AssetDatabase::skinSource() {
    return [this](const std::string& ref) -> std::shared_ptr<const SkinData> {
        AssetGuid guid = 0;
        u32 index = 0;
        if (!parseRef(ref, guid, index)) return nullptr;
        const AssetRecord* r = find(guid);
        if (!r || r->type != "Model") return nullptr;
        const AssetData* data = load(guid);
        const ModelData* model = data ? data->as<ModelData>() : nullptr;
        if (!model || index >= model->skins.size()) return nullptr;
        return model->skins[index];
    };
}

// -------------------------------------------------------------- references --
std::string AssetDatabase::makeRef(AssetGuid guid, u32 index) {
    return std::string(kRefPrefix) + toHex(guid) + "#" + std::to_string(index);
}

bool AssetDatabase::parseRef(std::string_view ref, AssetGuid& guid, u32& index) {
    if (!ref.starts_with(kRefPrefix)) return false;
    ref.remove_prefix(kRefPrefix.size());
    index = 0;
    if (const size_t hash = ref.find('#'); hash != std::string_view::npos) {
        const std::string_view idx = ref.substr(hash + 1);
        auto [ptr, ec] = std::from_chars(idx.data(), idx.data() + idx.size(), index);
        if (ec != std::errc() || ptr != idx.data() + idx.size()) return false;
        ref = ref.substr(0, hash);
    }
    return fromHex(ref, guid) && guid != 0;
}

std::string AssetDatabase::meshRef(MeshHandle mesh) const {
    auto it = m_meshOwners.find(mesh.id);
    return it == m_meshOwners.end() ? std::string{} : makeRef(it->second.first, it->second.second);
}

std::string AssetDatabase::textureRef(rhi::TextureHandle texture) const {
    auto it = m_textureOwners.find(texture.key());
    return it == m_textureOwners.end() ? std::string{} : makeRef(it->second.first, it->second.second);
}

MeshHandle AssetDatabase::findMesh(std::string_view ref) {
    AssetGuid guid = 0;
    u32 index = 0;
    if (!parseRef(ref, guid, index)) return m_renderer ? m_renderer->findMesh(std::string(ref)) : MeshHandle{};
    const AssetData* data = load(guid);
    return data && index < data->meshes.size() ? data->meshes[index] : MeshHandle{};
}

rhi::TextureHandle AssetDatabase::findTexture(std::string_view ref) {
    AssetGuid guid = 0;
    u32 index = 0;
    if (!parseRef(ref, guid, index)) return {};
    const AssetData* data = load(guid);
    return data && index < data->textures.size() ? data->textures[index] : rhi::TextureHandle{};
}

std::string AssetDatabase::textureLabel(rhi::TextureHandle texture) const {
    auto it = m_textureOwners.find(texture.key());
    if (it == m_textureOwners.end()) return {};
    const AssetRecord* r = find(it->second.first);
    std::string label = r ? r->path.generic_string() : makeRef(it->second.first);
    if (r && r->type != "Texture") label += " #" + std::to_string(it->second.second);
    return label;
}

std::string AssetDatabase::shaderRef(ShaderHandle shader) const {
    auto it = m_shaderOwners.find(shader.id);
    return it == m_shaderOwners.end() ? std::string{} : makeRef(it->second.first, it->second.second);
}

ShaderHandle AssetDatabase::findShader(std::string_view ref) {
    AssetGuid guid = 0;
    u32 index = 0;
    if (!parseRef(ref, guid, index)) return {};
    const AssetData* data = load(guid);
    return data && index < data->shaders.size() ? data->shaders[index] : ShaderHandle{};
}

std::string AssetDatabase::shaderLabel(ShaderHandle shader) const {
    auto it = m_shaderOwners.find(shader.id);
    if (it == m_shaderOwners.end()) return {};
    const AssetRecord* r = find(it->second.first);
    return r ? r->path.generic_string() : makeRef(it->second.first);
}

SerializeOptions AssetDatabase::serializeOptions() {
    SerializeOptions o;
    o.meshName = [this](MeshHandle m) {
        std::string ref = meshRef(m);
        if (ref.empty() && m_renderer) ref = m_renderer->meshName(m);
        return ref;
    };
    o.findMesh = [this](const std::string& ref) { return findMesh(ref); };
    o.textureName = [this](rhi::TextureHandle t) { return textureRef(t); };
    o.findTexture = [this](const std::string& ref) { return findTexture(ref); };
    o.shaderName = [this](ShaderHandle s) { return shaderRef(s); };
    o.findShader = [this](const std::string& ref) { return findShader(ref); };
    return o;
}

// ----------------------------------------------------------------- prefabs --
std::optional<AssetGuid> AssetDatabase::createPrefab(const std::vector<Entity>& entities, const fs::path& file,
                                                     std::string* error) {
    auto fail = [&](std::string msg) -> std::optional<AssetGuid> {
        if (error) *error = std::move(msg);
        return std::nullopt;
    };
    if (entities.empty()) return fail("nothing to save");
    if (m_root.empty()) return fail("the asset database has no root folder");
    fs::path path = file.is_absolute() ? file : m_root / file;
    if (!lower(path.filename().string()).ends_with(".prefab.json")) path += ".prefab.json";

    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    {
        std::ofstream out(path, std::ios::binary);
        if (!out) return fail("cannot write " + path.string());
        out << serializePrefab(entities, serializeOptions()) << '\n';
    }
    refresh();
    const AssetRecord* r = findByPath(path);
    if (!r) return fail("the prefab is outside the asset folder");
    unload(r->guid); // overwritten: reload next time
    return r->guid;
}

} // namespace nova
