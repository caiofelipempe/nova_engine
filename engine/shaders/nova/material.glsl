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
layout(location = 3) flat in uint vInstance;

layout(location = 0) out vec4 outColor;

struct Surface {
    vec3 albedo;      // linear base color
    float alpha;      // opacity (Material alphaMode Mask: cut below the cutoff; Blend: see-through)
    vec3 normal;      // world-space shading normal (unit, facing the camera)
    float metallic;   // 0..1
    float roughness;  // 0.04..1 (clamped after material())
    float occlusion;  // ambient occlusion, 1 = none
    vec3 emissive;    // HDR emitted radiance (already × strength)
    bool unlit;       // true: output albedo + emissive, skip lighting
    // Read-only inputs (changing them has no effect on lighting):
    vec3 worldPos;    // world position modulo NOVA_WORLD_TILE (4096 m): for patterns and noise
    vec3 relativePos; // position relative to the camera (exact; what lighting uses)
    vec2 uv;          // the mesh UV through the material's UV transform
    vec3 viewDir;     // unit vector from the surface to the camera
    vec4 custom;      // Material::customParams
};

// ---- helpers for material code
InstanceData nova_instance() { return pc.instances.items[vInstance]; }
float nova_time() { return pc.frame.data.cameraPos.w; }
vec3 nova_cameraPos() { return pc.frame.data.cameraPos.xyz; }
// Samples a bindless texture (index < 0 → white).
vec4 nova_texture(int index, vec2 uv) {
    return index >= 0 ? texture(g_textures[nonuniformEXT(index)], uv) : vec4(1.0);
}
// Material::customTexture (white when unset).
vec4 nova_customTexture(vec2 uv) { return nova_texture(int(nova_instance().extra.x), uv); }

// The normal tilted by a tangent-space normal map value (`tangentNormal`, −1..1) without stored
// tangents: the frame comes from the screen-space derivatives of position and UV (as the glTF
// sample viewer does for meshes without tangents).
vec3 nova_perturbNormal(vec3 N, vec3 position, vec2 uv, vec3 tangentNormal) {
    vec3 dpx = dFdx(position), dpy = dFdy(position);
    vec2 duvx = dFdx(uv), duvy = dFdy(uv);
    float det = duvx.x * duvy.y - duvy.x * duvx.y;
    if (abs(det) < 1e-12) return N;
    vec3 t = (duvy.y * dpx - duvx.y * dpy) / det;
    t = t - N * dot(N, t);
    if (dot(t, t) < 1e-20) return N;
    t = normalize(t);
    vec3 b = cross(N, t);
    return normalize(mat3(t, b, N) * tangentNormal);
}

// Implemented by the material shader.
void material(inout Surface s);

#include "nova/lighting.glsl"

void main() {
    InstanceData inst = nova_instance();
    uint matFlags = floatBitsToUint(inst.params.z);

    Surface s;
    s.normal = normalize(vNormal);
    if (!gl_FrontFacing) s.normal = -s.normal;
    s.relativePos = vWorldPos;
    s.worldPos = vWorldPos + pc.frame.data.originTiled.xyz;
    s.uv = nova_materialUv(inst, vUV); // through the material's UV transform
    s.viewDir = normalize(nova_cameraPos() - vWorldPos);
    s.custom = inst.custom;
    const vec4 base = inst.baseColor * nova_texture(int(inst.params.w), s.uv);
    s.albedo = base.rgb;
    s.alpha = base.a;
    if ((matFlags & MATERIAL_FLAG_CHECKER) != 0u) {
        vec2 c = floor(s.worldPos.xz);
        s.albedo *= mod(c.x + c.y, 2.0) < 1.0 ? 1.0 : 0.6;
    }
    // Metallic-roughness (glTF: G roughness, B metallic), occlusion (R), emissive and normal maps.
    const vec4 mr = nova_texture(int(inst.maps.y), s.uv);
    s.metallic = inst.params.x * mr.b;
    s.roughness = inst.params.y * mr.g;
    s.occlusion = inst.maps.z >= 0.0 ? mix(1.0, nova_texture(int(inst.maps.z), s.uv).r, inst.surface.y) : 1.0;
    s.emissive = inst.emissive.rgb * inst.emissive.w * nova_texture(int(inst.maps.w), s.uv).rgb;
    if (inst.maps.x >= 0.0) {
        vec3 tn = nova_texture(int(inst.maps.x), s.uv).xyz * 2.0 - 1.0;
        tn.xy *= inst.surface.x;
        s.normal = nova_perturbNormal(s.normal, vWorldPos, s.uv, normalize(tn));
    }
    s.unlit = false;

    material(s);

    const uint alphaMode = uint(inst.surface.w + 0.5);
    if (alphaMode == 1u && s.alpha < inst.surface.z) discard; // cut out

    vec3 N = normalize(s.normal);
    float metallic = clamp(s.metallic, 0.0, 1.0);
    float roughness = clamp(s.roughness, 0.04, 1.0);
    float sunShadow = 1.0;
    vec3 color = s.unlit ? s.albedo
                         : nova_shadeOccluded(vWorldPos, N, s.viewDir, s.albedo, metallic, roughness,
                                              clamp(s.occlusion, 0.0, 1.0), sunShadow);
    color += s.emissive;

    switch (pc.debugView) {
        case 1u: color = N * 0.5 + 0.5; break;
        case 2u: color = vec3(sunShadow); break;
        case 3u: color = s.albedo; break;
        default: break;
    }
    outColor = vec4(color, alphaMode == 2u ? clamp(s.alpha, 0.0, 1.0) : 1.0);
}

#endif // NOVA_MATERIAL_GLSL
