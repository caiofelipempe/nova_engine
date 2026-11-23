#include "nova/assets/GltfLoader.h"

#include <functional>

#include "nova/core/Log.h"
#include "nova/renderer/Renderer.h"

#define CGLTF_IMPLEMENTATION
#if defined(_MSC_VER)
    #pragma warning(push, 0)
#endif
#include <cgltf.h>
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
    #pragma warning(push, 0)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif

#include <cstring>
#include <fstream>

namespace nova {

namespace {

const cgltf_accessor* findAttribute(const cgltf_primitive& prim, cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < prim.attributes_count; ++i)
        if (prim.attributes[i].type == type) return prim.attributes[i].data;
    return nullptr;
}

bool buildMesh(const cgltf_primitive& prim, MeshData& out) {
    if (prim.type != cgltf_primitive_type_triangles) return false;
    const cgltf_accessor* pos = findAttribute(prim, cgltf_attribute_type_position);
    if (!pos) return false;
    const cgltf_accessor* nrm = findAttribute(prim, cgltf_attribute_type_normal);
    const cgltf_accessor* uv = findAttribute(prim, cgltf_attribute_type_texcoord);

    out.vertices.resize(pos->count);
    for (cgltf_size i = 0; i < pos->count; ++i) {
        Vertex& v = out.vertices[i];
        cgltf_accessor_read_float(pos, i, &v.position.x, 3);
        if (nrm) cgltf_accessor_read_float(nrm, i, &v.normal.x, 3);
        if (uv) {
            f32 t[2];
            cgltf_accessor_read_float(uv, i, t, 2);
            v.u = t[0];
            v.v = t[1];
        }
    }

    if (prim.indices) {
        out.indices.resize(prim.indices->count);
        for (cgltf_size i = 0; i < prim.indices->count; ++i)
            out.indices[i] = static_cast<u32>(cgltf_accessor_read_index(prim.indices, i));
    } else {
        out.indices.resize(pos->count);
        for (u32 i = 0; i < pos->count; ++i) out.indices[i] = i;
    }

    if (!nrm) { // flat-ish normals from faces
        for (auto& v : out.vertices) v.normal = glm::vec3(0.0f);
        for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
            Vertex& a = out.vertices[out.indices[i]];
            Vertex& b = out.vertices[out.indices[i + 1]];
            Vertex& c = out.vertices[out.indices[i + 2]];
            glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);
            a.normal += n;
            b.normal += n;
            c.normal += n;
        }
        for (auto& v : out.vertices) v.normal = glm::length(v.normal) > 0 ? glm::normalize(v.normal) : glm::vec3(0, 1, 0);
    }
    return !out.indices.empty();
}

Material buildMaterial(const cgltf_data* data, const cgltf_material* m, const std::vector<rhi::TextureHandle>& images) {
    Material mat;
    if (!m) return mat;
    if (m->has_pbr_metallic_roughness) {
        const auto& pbr = m->pbr_metallic_roughness;
        mat.baseColor = glm::make_vec4(pbr.base_color_factor);
        mat.metallic = pbr.metallic_factor;
        mat.roughness = pbr.roughness_factor;
        if (const cgltf_texture* tex = pbr.base_color_texture.texture; tex && tex->image) {
            const size_t index = static_cast<size_t>(tex->image - data->images);
            if (index < images.size()) mat.baseColorTexture = images[index];
        }
    }
    mat.emissive = glm::make_vec3(m->emissive_factor);
    mat.emissiveStrength = (mat.emissive != glm::vec3(0.0f)) ? (m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f) : 0.0f;
    return mat;
}

// Relative URI of an external resource → path next to the glTF (percent-decoded).
std::filesystem::path resolveUri(const std::filesystem::path& gltf, const char* uri) {
    std::string decoded = uri;
    cgltf_decode_uri(decoded.data());
    decoded.resize(std::strlen(decoded.c_str()));
    return gltf.parent_path() / std::filesystem::path(std::u8string(decoded.begin(), decoded.end()));
}

bool isExternal(const char* uri) { return uri && std::strncmp(uri, "data:", 5) != 0 && !std::strstr(uri, "://"); }

rhi::TextureHandle loadImage(Renderer& renderer, const std::filesystem::path& gltf, const cgltf_image& image, size_t index) {
    std::vector<u8> pixels;
    u32 w = 0, h = 0;
    if (image.buffer_view && image.buffer_view->buffer->data) {
        const u8* bytes = static_cast<const u8*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
        pixels = decodeImageMemory(bytes, image.buffer_view->size, w, h);
    } else if (isExternal(image.uri)) {
        pixels = decodeImageFile(resolveUri(gltf, image.uri), w, h);
    }
    if (pixels.empty()) {
        NOVA_CORE_WARN("glTF: image {} of '{}' could not be loaded", index, gltf.string());
        return {};
    }
    const std::string name = image.name ? image.name : gltf.stem().string() + "/image" + std::to_string(index);
    return renderer.createTexture2D(w, h, pixels.data(), name.c_str());
}

struct CgltfFile {
    cgltf_data* data = nullptr;
    ~CgltfFile() { if (data) cgltf_free(data); }
};

bool parse(const std::filesystem::path& path, CgltfFile& file, bool loadBuffers) {
    cgltf_options options{};
    const std::string p = path.string();
    if (cgltf_parse_file(&options, p.c_str(), &file.data) != cgltf_result_success) {
        NOVA_CORE_ERROR("glTF: failed to parse '{}'", p);
        return false;
    }
    if (loadBuffers && cgltf_load_buffers(&options, file.data, p.c_str()) != cgltf_result_success) {
        NOVA_CORE_ERROR("glTF: failed to load buffers for '{}'", p);
        return false;
    }
    return true;
}

} // namespace

std::vector<u8> decodeImageMemory(const void* data, size_t size, u32& width, u32& height) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(data), static_cast<int>(size), &w, &h, &channels, 4);
    if (!pixels) return {};
    std::vector<u8> out(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(pixels);
    width = static_cast<u32>(w);
    height = static_cast<u32>(h);
    return out;
}

std::vector<u8> decodeImageFile(const std::filesystem::path& path, u32& width, u32& height) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return decodeImageMemory(bytes.data(), bytes.size(), width, height);
}

std::optional<ModelData> loadModel(Renderer& renderer, const std::filesystem::path& path, const std::string& meshPrefix) {
    CgltfFile file;
    if (!parse(path, file, true)) return std::nullopt;
    const cgltf_data* data = file.data;
    const std::string prefix = meshPrefix.empty() ? path.stem().string() + "/" : meshPrefix;

    ModelData model;
    model.name = path.stem().string();
    for (cgltf_size i = 0; i < data->images_count; ++i) model.textures.push_back(loadImage(renderer, path, data->images[i], i));

    // One GPU mesh per (mesh, primitive), shared by all nodes that instance it. ~0u = unusable.
    std::vector<std::vector<u32>> meshCache(data->meshes_count);
    auto meshesOf = [&](const cgltf_node& node) -> const std::vector<u32>& {
        const cgltf_size meshIndex = static_cast<cgltf_size>(node.mesh - data->meshes);
        auto& cache = meshCache[meshIndex];
        if (cache.empty())
            for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
                MeshData md;
                std::string name = prefix + (node.mesh->name ? node.mesh->name : ("mesh" + std::to_string(meshIndex)));
                if (node.mesh->primitives_count > 1) name += "#" + std::to_string(pi);
                if (buildMesh(node.mesh->primitives[pi], md)) {
                    cache.push_back(static_cast<u32>(model.meshes.size()));
                    model.meshes.push_back(renderer.createMesh(md, name));
                } else {
                    cache.push_back(~0u);
                }
            }
        return cache;
    };

    // Every node, depth first from the scene's roots: parents before children. A node gets its
    // mesh's first primitive; the others become children of it, at the same pose.
    std::vector<bool> visited(data->nodes_count, false);
    std::function<void(const cgltf_node&, u32)> visit = [&](const cgltf_node& node, u32 parent) {
        const cgltf_size n = static_cast<cgltf_size>(&node - data->nodes);
        if (visited[n]) return;
        visited[n] = true;
        f32 world[16];
        cgltf_node_transform_world(&node, world);
        glm::vec3 scale, translation, skew;
        glm::vec4 perspective;
        glm::quat rotation;
        glm::decompose(glm::make_mat4(world), scale, rotation, translation, skew, perspective);

        ModelNode mn;
        mn.name = node.name ? node.name : ("node" + std::to_string(n));
        mn.position = translation;
        mn.rotation = rotation;
        mn.scale = scale;
        mn.parent = parent;
        std::vector<std::pair<u32, cgltf_size>> primitives; // (mesh, primitive) usable
        if (node.mesh) {
            const std::vector<u32>& cache = meshesOf(node);
            for (size_t pi = 0; pi < cache.size(); ++pi)
                if (cache[pi] != ~0u) primitives.emplace_back(cache[pi], pi);
        }
        if (!primitives.empty()) {
            mn.mesh = primitives[0].first;
            mn.material = buildMaterial(data, node.mesh->primitives[primitives[0].second].material, model.textures);
        }
        const u32 self = static_cast<u32>(model.nodes.size());
        model.nodes.push_back(mn);
        for (size_t k = 1; k < primitives.size(); ++k) {
            ModelNode extra = mn;
            extra.name = mn.name + "#" + std::to_string(primitives[k].second);
            extra.mesh = primitives[k].first;
            extra.material = buildMaterial(data, node.mesh->primitives[primitives[k].second].material, model.textures);
            extra.parent = self;
            model.nodes.push_back(std::move(extra));
        }
        for (cgltf_size c = 0; c < node.children_count; ++c) visit(*node.children[c], self);
    };
    if (data->scene)
        for (cgltf_size r = 0; r < data->scene->nodes_count; ++r) visit(*data->scene->nodes[r], ~0u);
    for (cgltf_size n = 0; n < data->nodes_count; ++n) // no scene, or nodes outside it: their own roots
        if (!data->nodes[n].parent) visit(data->nodes[n], ~0u);

    NOVA_CORE_INFO("glTF: loaded '{}' ({} meshes, {} textures, {} nodes)", path.string(), model.meshes.size(),
                   model.textures.size(), model.nodes.size());
    return model;
}

std::vector<Entity> instantiateModel(Scene& scene, const ModelData& model, const GlobalPosition& position) {
    std::vector<Entity> out;
    Entity root = scene.createEntity(model.name.empty() ? "Model" : model.name);
    root.transform().position = position;
    out.push_back(root);
    std::vector<Entity> made;
    made.reserve(model.nodes.size());
    for (const ModelNode& n : model.nodes) {
        Entity e = scene.createEntity(n.name);
        auto& t = e.transform();
        t.position = position + n.position; // its pose in the world: parenting keeps it
        t.setRotation(n.rotation);
        t.scale = n.scale;
        if (n.mesh < model.meshes.size()) e.add<MeshComponent>(model.meshes[n.mesh], n.material, true);
        made.push_back(e);
        out.push_back(e);
    }
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const u32 parent = model.nodes[i].parent;
        scene.setParent(made[i], parent < i ? made[parent] : root);
    }
    return out;
}

std::vector<std::filesystem::path> gltfDependencies(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> deps;
    CgltfFile file;
    if (!parse(path, file, false)) return deps;
    for (cgltf_size i = 0; i < file.data->buffers_count; ++i)
        if (isExternal(file.data->buffers[i].uri)) deps.push_back(resolveUri(path, file.data->buffers[i].uri));
    for (cgltf_size i = 0; i < file.data->images_count; ++i)
        if (isExternal(file.data->images[i].uri)) deps.push_back(resolveUri(path, file.data->images[i].uri));
    return deps;
}

std::optional<Entity> loadGltf(Scene& scene, Renderer& renderer, const std::filesystem::path& path) {
    auto model = loadModel(renderer, path);
    if (!model) return std::nullopt;
    auto entities = instantiateModel(scene, *model);
    if (entities.empty()) return std::nullopt;
    return entities.front(); // the root
}

} // namespace nova