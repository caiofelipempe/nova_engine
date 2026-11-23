// Engine lighting: Cook-Torrance GGX, ray-queried shadows and the hemisphere ambient.
// Included by nova/material.glsl; needs `pc` (nova/forward_push.glsl) and, for ray-traced
// shadows, NOVA_RAY_QUERY + GL_EXT_ray_query (the renderer compiles that variant itself).
#ifndef NOVA_LIGHTING_GLSL
#define NOVA_LIGHTING_GLSL
#include "nova/forward_push.glsl"

const float NOVA_PI = 3.14159265359;

float nova_D_GGX(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (NOVA_PI * d * d);
}

float nova_G_SchlickGGX(float NdotX, float k) { return NdotX / (NdotX * (1.0 - k) + k); }

vec3 nova_F_Schlick(float VdotH, vec3 F0) { return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0); }

// Cook-Torrance GGX; returns the radiance factor already multiplied by N.L.
vec3 nova_brdf(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float a = max(roughness * roughness, 0.002);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = nova_F_Schlick(VdotH, F0);
    float D = nova_D_GGX(NdotH, a);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float G = nova_G_SchlickGGX(NdotV, k) * nova_G_SchlickGGX(NdotL, k);

    vec3 specular = D * G * F / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / NOVA_PI + specular) * NdotL;
}

#ifdef NOVA_RAY_QUERY
// A shadow ray met a triangle of a cut-out material (alpha Mask, non-opaque in the TLAS): does it
// block the light there? Its alpha at that point (base color × texture, through the UV transform).
bool nova_shadowAlphaBlocks(uint instance, uint primitive, vec2 bary) {
    InstanceData inst = pc.instances.items[instance];
    VertexBuffer vertices = VertexBuffer(packUint2x32(inst.mesh.xy));
    IndexBuffer indices = IndexBuffer(packUint2x32(inst.mesh.zw));
    if (uint64_t(vertices) == 0ul || uint64_t(indices) == 0ul) return true;
    Vertex a = vertices.items[indices.items[primitive * 3u + 0u]];
    Vertex b = vertices.items[indices.items[primitive * 3u + 1u]];
    Vertex c = vertices.items[indices.items[primitive * 3u + 2u]];
    vec2 uv = vec2(a.u, a.v) * (1.0 - bary.x - bary.y) + vec2(b.u, b.v) * bary.x + vec2(c.u, c.v) * bary.y;
    uv = nova_materialUv(inst, uv);
    int tex = int(inst.params.w);
    float alpha = inst.baseColor.a * (tex >= 0 ? textureLod(g_textures[nonuniformEXT(tex)], uv, 0.0).a : 1.0);
    return alpha >= inst.surface.z;
}
#endif

// 1 = lit, 0 = occluded. Without ray query support this is a no-op (future: shadow maps).
float nova_traceShadow(vec3 origin, vec3 dir, float tMax) {
#ifdef NOVA_RAY_QUERY
    if (pc.tlas == 0ul) return 1.0;
    rayQueryEXT rq;
    // Opaque instances are opaque in the TLAS; cut-out ones are tested here, by their alpha.
    rayQueryInitializeEXT(rq, accelerationStructureEXT(pc.tlas), gl_RayFlagsTerminateOnFirstHitEXT, 0xFF,
                          origin, 0.0, dir, tMax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            nova_shadowAlphaBlocks(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)),
                                   uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)),
                                   rayQueryGetIntersectionBarycentricsEXT(rq, false)))
            rayQueryConfirmIntersectionEXT(rq);
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
#else
    return 1.0;
#endif
}

// The sun's visibility from the shadow maps (cascades by distance, 3 × 3 filtered): 1 = lit.
float nova_shadowMaps(vec3 P, vec3 N) {
    float along = dot(P, pc.frame.data.cameraForward.xyz);
    int c = 0;
    while (c < 4 && along > pc.frame.data.shadowSplits[c]) ++c;
    if (c >= 4) return 1.0; // beyond the shadow distance
    vec3 offsetP = P + N * pc.frame.data.shadowTexels[c] * 1.5; // off the surface by a texel or so
    vec4 clip = pc.frame.data.shadowMatrices[c] * vec4(offsetP, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || ndc.z > 1.0) return 1.0;
    int atlas = int(pc.frame.data.shadowParams.x);
    int res = int(pc.frame.data.shadowParams.z);
    ivec2 tile = ivec2(c & 1, c >> 1) * res;
    ivec2 at = tile + clamp(ivec2(uv * float(res)), ivec2(0), ivec2(res - 1));
    float depth = ndc.z - pc.frame.data.shadowParams.y;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            ivec2 q = clamp(at + ivec2(x, y), tile, tile + ivec2(res - 1));
            lit += depth <= texelFetch(g_textures[nonuniformEXT(atlas)], q, 0).r ? 1.0 : 0.0;
        }
    return lit / 9.0;
}

// Point light shadow maps: six cube faces per light in a row of the atlas, each a 90° view along
// kNovaFaceDir with kNovaFaceUp (as Renderer's), depth from the light's near plane to its radius.
const vec3 kNovaFaceDir[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
const vec3 kNovaFaceUp[6] = vec3[6](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, -1), vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1, 0));

// A point light's visibility from its shadow map (3 × 3 filtered): 1 = lit.
float nova_pointShadowMap(int slot, vec3 P, vec3 N, vec3 lightPos, float radius) {
    const vec4 prm = pc.frame.data.pointShadowParams;
    const int atlas = int(prm.x), res = int(prm.y);
    const float near = prm.z;
    const float dist = length(P - lightPos);
    const vec3 d = P + N * (2.0 * dist / float(res)) * 1.5 - lightPos; // off the surface by a texel or so
    const vec3 a = abs(d);
    const int face = a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0 : 1) : a.y >= a.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5);
    const vec3 f = kNovaFaceDir[face];
    const vec3 s = normalize(cross(f, kNovaFaceUp[face]));
    const vec3 u = cross(s, f);
    const float m = max(dot(f, d), 1e-4);                   // distance along the face's axis
    const vec2 uv = vec2(dot(s, d), -dot(u, d)) / m * 0.5 + 0.5; // y flipped, as the projection
    const float depth = radius / (radius - near) - radius * near / ((radius - near) * m) - prm.w;
    const ivec2 tile = ivec2(face, slot) * res;
    const ivec2 at = tile + clamp(ivec2(uv * float(res)), ivec2(0), ivec2(res - 1));
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const ivec2 q = clamp(at + ivec2(x, y), tile, tile + ivec2(res - 1));
            lit += depth <= texelFetch(g_textures[nonuniformEXT(atlas)], q, 0).r ? 1.0 : 0.0;
        }
    return lit / 9.0;
}

// Shades a surface with the sun, the point lights and the ambient (darkened by `occlusion`);
// `sunShadow` receives the sun visibility (for the Shadows debug view).
vec3 nova_shadeOccluded(vec3 P, vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, float occlusion,
                        out float sunShadow) {
    bool rtShadows = (pc.frame.data.flags & FRAME_FLAG_RT_SHADOWS) != 0u;
    float bias = pc.frame.data.shadowBias;
    vec3 origin = P + N * bias;

    vec3 color = vec3(0.0);
    sunShadow = 1.0;

    // Directional light (sun)
    vec3 Ls = -normalize(pc.frame.data.sunDirection.xyz);
    if (dot(N, Ls) > 0.0) {
        if (rtShadows) sunShadow = nova_traceShadow(origin, Ls, 10000.0);
        else if ((pc.frame.data.flags & FRAME_FLAG_SHADOW_MAPS) != 0u) sunShadow = nova_shadowMaps(P, N);
        color += nova_brdf(N, V, Ls, albedo, metallic, roughness) * pc.frame.data.sunColor.rgb *
                 pc.frame.data.sunDirection.w * sunShadow;
    }

    // Point lights
    uint count = min(pc.frame.data.pointLightCount, uint(MAX_POINT_LIGHTS));
    for (uint i = 0u; i < count; ++i) {
        PointLight light = pc.frame.data.pointLights[i];
        vec3 toLight = light.positionRadius.xyz - P;
        float dist = length(toLight);
        float radius = light.positionRadius.w;
        if (dist >= radius) continue;
        vec3 L = toLight / dist;
        if (dot(N, L) <= 0.0) continue;
        float window = clamp(1.0 - pow(dist / radius, 4.0), 0.0, 1.0);
        float atten = window * window / (dist * dist + 1.0);
        const int slot = pc.frame.data.pointShadowSlots[i >> 2u][i & 3u];
        float shadow = 1.0;
        if (slot == -2) shadow = 1.0; // casts no shadows
        else if (rtShadows) shadow = nova_traceShadow(origin, L, max(dist - bias, 0.0));
        else if (slot >= 0 && (pc.frame.data.flags & FRAME_FLAG_POINT_SHADOW_MAPS) != 0u)
            shadow = nova_pointShadowMap(slot, P, N, light.positionRadius.xyz, radius);
        color += nova_brdf(N, V, L, albedo, metallic, roughness) * light.colorIntensity.rgb *
                 light.colorIntensity.w * atten * shadow;
    }

    // Hemisphere ambient
    vec3 sky = pc.frame.data.skyZenith.rgb;
    vec3 ground = pc.frame.data.groundColor.rgb;
    vec3 ambient = mix(ground, sky, N.y * 0.5 + 0.5) * pc.frame.data.skyZenith.w;
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    color += ambient * (albedo * (1.0 - metallic) + F0 * 0.25) * occlusion;
    return color;
}

// Same, without ambient occlusion.
vec3 nova_shade(vec3 P, vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, out float sunShadow) {
    return nova_shadeOccluded(P, N, V, albedo, metallic, roughness, 1.0, sunShadow);
}

#endif // NOVA_LIGHTING_GLSL
