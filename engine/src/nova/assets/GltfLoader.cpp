#include "nova/assets/GltfLoader.h"

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

Material buildMaterial(const cgltf_material* m) {
    Material mat;
    if (!m) return mat;
    if (m->has_pbr_metallic_roughness) {
        const auto& pbr = m->pbr_metallic_roughness;
        mat.baseColor = glm::make_vec4(pbr.base_color_factor);
        mat.metallic = pbr.metallic_factor;
        mat.roughness = pbr.roughness_factor;
    }
    mat.emissive = glm::make_vec3(m->emissive_factor);
    mat.emissiveStrength = (mat.emissive != glm::vec3(0.0f)) ? (m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f) : 0.0f;
    return mat;
}

} // namespace

std::optional<Entity> loadGltf(Scene& scene, Renderer& renderer, const std::filesystem::path& path) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const std::string p = path.string();
    if (cgltf_parse_file(&options, p.c_str(), &data) != cgltf_result_success) {
        NOVA_CORE_ERROR("glTF: failed to parse '{}'", p);
        return std::nullopt;
    }
    if (cgltf_load_buffers(&options, data, p.c_str()) != cgltf_result_success) {
        NOVA_CORE_ERROR("glTF: failed to load buffers for '{}'", p);
        cgltf_free(data);
        return std::nullopt;
    }

    Entity root = scene.createEntity(path.stem().string());

    // One GPU mesh per (mesh, primitive), shared by all nodes that instance it.
    std::vector<std::vector<MeshHandle>> meshCache(data->meshes_count);
    u32 created = 0;

    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh) continue;
        const cgltf_size meshIndex = static_cast<cgltf_size>(node.mesh - data->meshes);
        auto& cache = meshCache[meshIndex];
        if (cache.empty()) {
            for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
                MeshData md;
                std::string name = node.mesh->name ? node.mesh->name : ("mesh" + std::to_string(meshIndex));
                if (node.mesh->primitives_count > 1) name += "#" + std::to_string(pi);
                cache.push_back(buildMesh(node.mesh->primitives[pi], md) ? renderer.createMesh(md, name)
                                                                           : MeshHandle{});
            }
        }

        f32 world[16];
        cgltf_node_transform_world(&node, world);
        glm::mat4 m = glm::make_mat4(world);
        glm::vec3 scale, translation, skew;
        glm::vec4 perspective;
        glm::quat rotation;
        glm::decompose(m, scale, rotation, translation, skew, perspective);

        for (cgltf_size pi = 0; pi < cache.size(); ++pi) {
            if (!cache[pi]) continue;
            std::string name = node.name ? node.name : ("node" + std::to_string(n));
            Entity e = scene.createEntity(name);
            auto& t = e.transform();
            t.position = translation;
            t.rotation = glm::eulerAngles(rotation);
            t.scale = scale;
            e.add<MeshComponent>(cache[pi], buildMaterial(node.mesh->primitives[pi].material), true);
            ++created;
        }
    }

    cgltf_free(data);
    NOVA_CORE_INFO("glTF: imported '{}' ({} entities)", p, created);
    return root;
}

} // namespace nova
