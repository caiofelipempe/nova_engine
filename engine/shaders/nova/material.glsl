// Material shader interface. A game material is a fragment shader that only changes the surface;
// lighting, ray-traced shadows and debug views stay the engine's:
//
//   #version 460
//   #include "nova/material.glsl"
//
//   void material(inout Surface s) {
//       float stripes = step(0.5, fract(s.worldPos.y * 4.0 + nova_time()));
//       s.albedo *= mix(vec3(1.0), s.custom.rgb, stripes);   // Material::customParams
//   }
//
// `s` arrives filled from the Material (base color × texture, checker, metallic, roughness,
// emissive). The engine's own forward.frag is exactly this file with an empty material().
// The renderer also builds a ray-query variant of every material (NOVA_RAY_QUERY defined).
#ifndef NOVA_MATERIAL_GLSL
#define NOVA_MATERIAL_GLSL
#ifdef NOVA_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif
#include "nova/common.glsl"
#include "nova/forward_push.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

struct Surface {
    vec3 albedo;      // linear base color
    vec3 normal;      // world-space shading normal (unit, facing the camera)
    float metallic;   // 0..1
    float roughness;  // 0.04..1 (clamped after material())
    vec3 emissive;    // HDR emitted radiance (already × strength)
    bool unlit;       // true: output albedo + emissive, skip lighting
    // Read-only inputs (changing them has no effect on lighting):
    vec3 worldPos;
    vec2 uv;
    vec3 viewDir;     // unit vector from the surface to the camera
    vec4 custom;      // Material::customParams
};

// ---- helpers for material code
InstanceData nova_instance() { return pc.instances.items[pc.instanceIndex]; }
float nova_time() { return pc.frame.data.cameraPos.w; }
vec3 nova_cameraPos() { return pc.frame.data.cameraPos.xyz; }
// Samples a bindless texture (index < 0 → white).
vec4 nova_texture(int index, vec2 uv) {
    return index >= 0 ? texture(g_textures[nonuniformEXT(index)], uv) : vec4(1.0);
}
// Material::customTexture (white when unset).
vec4 nova_customTexture(vec2 uv) { return nova_texture(int(nova_instance().extra.x), uv); }

// Implemented by the material shader.
void material(inout Surface s);

#include "nova/lighting.glsl"

void main() {
    InstanceData inst = nova_instance();
    uint matFlags = floatBitsToUint(inst.params.z);

    Surface s;
    s.normal = normalize(vNormal);
    if (!gl_FrontFacing) s.normal = -s.normal;
    s.worldPos = vWorldPos;
    s.uv = vUV;
    s.viewDir = normalize(nova_cameraPos() - vWorldPos);
    s.custom = inst.custom;
    s.albedo = inst.baseColor.rgb * nova_texture(int(inst.params.w), vUV).rgb;
    if ((matFlags & MATERIAL_FLAG_CHECKER) != 0u) {
        vec2 c = floor(vWorldPos.xz);
        s.albedo *= mod(c.x + c.y, 2.0) < 1.0 ? 1.0 : 0.6;
    }
    s.metallic = inst.params.x;
    s.roughness = inst.params.y;
    s.emissive = inst.emissive.rgb * inst.emissive.w;
    s.unlit = false;

    material(s);

    vec3 N = normalize(s.normal);
    float metallic = clamp(s.metallic, 0.0, 1.0);
    float roughness = clamp(s.roughness, 0.04, 1.0);
    float sunShadow = 1.0;
    vec3 color = s.unlit ? s.albedo : nova_shade(vWorldPos, N, s.viewDir, s.albedo, metallic, roughness, sunShadow);
    color += s.emissive;

    switch (pc.debugView) {
        case 1u: color = N * 0.5 + 0.5; break;
        case 2u: color = vec3(sunShadow); break;
        case 3u: color = s.albedo; break;
        default: break;
    }
    outColor = vec4(color, 1.0);
}

#endif // NOVA_MATERIAL_GLSL
