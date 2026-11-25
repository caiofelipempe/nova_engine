// Ray tracing pipeline shaders (ray generation, miss, closest hit, any hit). C++: gpu::RtPush.
// Public: game shaders include it too (Renderer::rayTracingPipeline; the push's inputs[] and
// params are theirs; the engine's rt_alpha.rahit and rt_shadow.rmiss load as precompiled shaders).
// The frame's TLAS has one instance per drawn object (instanceCustomIndex = its InstanceData);
// the pipelines have two ray types: 0 = primary (hit group 0, miss 0), 1 = shadow (hit group 1,
// miss 1): traceRayEXT(..., sbtRecordOffset = ray type, sbtRecordStride = 2, missIndex = ray type).
#ifndef NOVA_RAYTRACING_GLSL
#define NOVA_RAYTRACING_GLSL
#extension GL_EXT_ray_tracing : require
#include "nova/common.glsl"

layout(push_constant) uniform RtPush {
    FrameBuffer frame;
    InstanceBuffer instances;
    uint64_t tlas;
    uint outputImage; // storage image (set 0, binding 1)
    float bias;       // offset of secondary rays along the normal (m)
    uint sampleIndex; // path tracer: samples already in the image (0 = start over)
    uint bounces;     // path tracer: bounces after the first hit
    uint inputs[4];   // game shaders: free bindless indices (textures, images)
    uint _pad0, _pad1;
    vec4 params;      // game shaders: free parameters
} pc;

// Bindless storage images (set 0, binding 1).
layout(set = 0, binding = 1, rgba16f) uniform image2D g_images[];

#define NOVA_RAY_PRIMARY 0u
#define NOVA_RAY_SHADOW 1u
#define NOVA_RAY_TYPES 2u

struct NovaPrimaryPayload {
    vec3 color; // radiance along the ray
    float hitT; // distance to the hit, < 0 = missed
};

// What the path tracer's closest hit reports: the surface, shaded by the ray generation shader.
struct NovaSurfacePayload {
    vec3 position;
    float hitT; // < 0 = missed
    vec3 normal;
    float metallic;
    vec3 albedo;
    float roughness;
    vec3 emissive;
    float _pad;
};

// What a ray hit: the triangle's interpolated attributes, through the instance's transform.
struct NovaHit {
    vec3 position;  // world (camera relative)
    vec3 normal;    // world, unit, facing the ray
    vec2 uv;        // through the material's UV transform
};

NovaHit nova_hitSurface(InstanceData inst, uint primitive, vec2 bary, vec3 rayOrigin, vec3 rayDir, float t) {
    VertexBuffer vertices = VertexBuffer(packUint2x32(inst.mesh.xy));
    IndexBuffer indices = IndexBuffer(packUint2x32(inst.mesh.zw));
    Vertex a = vertices.items[indices.items[primitive * 3u + 0u]];
    Vertex b = vertices.items[indices.items[primitive * 3u + 1u]];
    Vertex c = vertices.items[indices.items[primitive * 3u + 2u]];
    const vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    NovaHit h;
    h.position = rayOrigin + rayDir * t;
    h.normal = normalize(mat3(inst.normalMatrix) * (a.normal * w.x + b.normal * w.y + c.normal * w.z));
    if (dot(h.normal, rayDir) > 0.0) h.normal = -h.normal;
    h.uv = nova_materialUv(inst, vec2(a.u, a.v) * w.x + vec2(b.u, b.v) * w.y + vec2(c.u, c.v) * w.z);
    return h;
}

// The material at a hit: base color (and its texture), checker, metallic-roughness, emissive.
struct NovaHitMaterial {
    vec3 albedo;
    float metallic;
    vec3 emissive;
    float roughness;
};

NovaHitMaterial nova_hitMaterial(InstanceData inst, NovaHit hit, vec3 originTiled) {
    NovaHitMaterial m;
    const int tex = int(inst.params.w);
    m.albedo = inst.baseColor.rgb * (tex >= 0 ? textureLod(g_textures[nonuniformEXT(tex)], hit.uv, 0.0).rgb : vec3(1.0));
    if ((floatBitsToUint(inst.params.z) & MATERIAL_FLAG_CHECKER) != 0u) {
        const vec2 cell = floor((hit.position + originTiled).xz);
        m.albedo *= mod(cell.x + cell.y, 2.0) < 1.0 ? 1.0 : 0.6;
    }
    const int mrTex = int(inst.maps.y);
    const vec4 mr = mrTex >= 0 ? textureLod(g_textures[nonuniformEXT(mrTex)], hit.uv, 0.0) : vec4(1.0);
    m.metallic = clamp(inst.params.x * mr.b, 0.0, 1.0);
    m.roughness = clamp(inst.params.y * mr.g, 0.04, 1.0);
    const int emTex = int(inst.maps.w);
    m.emissive = inst.emissive.rgb * inst.emissive.w *
                 (emTex >= 0 ? textureLod(g_textures[nonuniformEXT(emTex)], hit.uv, 0.0).rgb : vec3(1.0));
    return m;
}

// Cut-out materials (alpha Mask): false where the surface is transparent (the any hit ignores it).
bool nova_alphaPasses(InstanceData inst, uint primitive, vec2 bary) {
    if (uint(inst.surface.w + 0.5) != 1u) return true;
    VertexBuffer vertices = VertexBuffer(packUint2x32(inst.mesh.xy));
    IndexBuffer indices = IndexBuffer(packUint2x32(inst.mesh.zw));
    Vertex a = vertices.items[indices.items[primitive * 3u + 0u]];
    Vertex b = vertices.items[indices.items[primitive * 3u + 1u]];
    Vertex c = vertices.items[indices.items[primitive * 3u + 2u]];
    vec2 uv = vec2(a.u, a.v) * (1.0 - bary.x - bary.y) + vec2(b.u, b.v) * bary.x + vec2(c.u, c.v) * bary.y;
    uv = nova_materialUv(inst, uv);
    const int tex = int(inst.params.w);
    const float alpha = inst.baseColor.a * (tex >= 0 ? textureLod(g_textures[nonuniformEXT(tex)], uv, 0.0).a : 1.0);
    return alpha >= inst.surface.z;
}

#endif // NOVA_RAYTRACING_GLSL
