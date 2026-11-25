// Engine lighting: Cook-Torrance GGX, ray-queried shadows, reflections and AO, and the hemisphere ambient.
// Included by nova/material.glsl; needs `pc` (nova/forward_push.glsl) and, for ray-traced
// shadows, NOVA_RAY_QUERY + GL_EXT_ray_query (the renderer compiles that variant itself).
#ifndef NOVA_LIGHTING_GLSL
#define NOVA_LIGHTING_GLSL
#include "nova/forward_push.glsl"
#include "nova/brdf.glsl"
#include "nova/sky.glsl"

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

// The sun's visibility from one cascade (3 × 3 filtered): 1 = lit.
float nova_shadowCascade(int c, vec3 P, vec3 N) {
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

// The sun's visibility from the shadow maps (cascades by distance): 1 = lit. In the last part of
// each cascade (shadowParams.w of its length) it blends into the next one, and the last cascade
// fades out toward the shadow distance, so no edge shows where one ends.
float nova_shadowMaps(vec3 P, vec3 N) {
    const float along = dot(P, pc.frame.data.cameraForward.xyz);
    int c = 0;
    while (c < 4 && along > pc.frame.data.shadowSplits[c]) ++c;
    if (c >= 4) return 1.0; // beyond the shadow distance
    float lit = nova_shadowCascade(c, P, N);
    const float blend = pc.frame.data.shadowParams.w;
    if (blend > 0.0) {
        const float start = c == 0 ? 0.0 : pc.frame.data.shadowSplits[c - 1], end = pc.frame.data.shadowSplits[c];
        const float band = max((end - start) * blend, 1e-3);
        const float t = (along - (end - band)) / band;
        if (t > 0.0) lit = mix(lit, c + 1 < 4 ? nova_shadowCascade(c + 1, P, N) : 1.0, smoothstep(0.0, 1.0, t));
    }
    return lit;
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

#ifdef NOVA_RAY_QUERY
// A per-pixel value in 0..1 that turns every frame when `frame` does (interleaved gradient noise).
float nova_noise(vec2 pixel, float frame) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))) + frame * 0.61803399);
}

// The nearest surface along a ray (cut-outs tested by alpha). False on a miss.
bool nova_traceClosest(vec3 origin, vec3 dir, float tMax, out uint instance, out uint primitive, out vec2 bary, out float t) {
    instance = primitive = 0u;
    bary = vec2(0.0);
    t = tMax;
    if (pc.tlas == 0ul) return false;
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, accelerationStructureEXT(pc.tlas), gl_RayFlagsNoneEXT, 0xFF, origin, 0.0, dir, tMax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            nova_shadowAlphaBlocks(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)),
                                   uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)),
                                   rayQueryGetIntersectionBarycentricsEXT(rq, false)))
            rayQueryConfirmIntersectionEXT(rq);
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return false;
    instance = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    t = rayQueryGetIntersectionTEXT(rq, true);
    return true;
}

// The light coming back along a reflected ray: the sky on a miss; on a hit, the surface there
// (base color and its texture, checker, emissive) lit simply: the sun (with a shadow ray) and the
// hemisphere ambient, as diffuse.
vec3 nova_reflectedRadiance(vec3 origin, vec3 dir) {
    uint instance, primitive;
    vec2 bary;
    float t;
    if (!nova_traceClosest(origin, dir, 1e4, instance, primitive, bary, t)) return nova_skyColor(pc.frame, dir);
    const InstanceData inst = pc.instances.items[instance];
    VertexBuffer vertices = VertexBuffer(packUint2x32(inst.mesh.xy));
    IndexBuffer indices = IndexBuffer(packUint2x32(inst.mesh.zw));
    const Vertex a = vertices.items[indices.items[primitive * 3u + 0u]];
    const Vertex b = vertices.items[indices.items[primitive * 3u + 1u]];
    const Vertex c = vertices.items[indices.items[primitive * 3u + 2u]];
    const vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    vec3 N = normalize(mat3(inst.normalMatrix) * (a.normal * w.x + b.normal * w.y + c.normal * w.z));
    if (dot(N, dir) > 0.0) N = -N;
    const vec2 uv = nova_materialUv(inst, vec2(a.u, a.v) * w.x + vec2(b.u, b.v) * w.y + vec2(c.u, c.v) * w.z);
    const vec3 P = origin + dir * t;
    const int tex = int(inst.params.w);
    vec3 albedo = inst.baseColor.rgb * (tex >= 0 ? textureLod(g_textures[nonuniformEXT(tex)], uv, 0.0).rgb : vec3(1.0));
    if ((floatBitsToUint(inst.params.z) & MATERIAL_FLAG_CHECKER) != 0u) {
        const vec2 cell = floor((P + pc.frame.data.originTiled.xyz).xz);
        albedo *= mod(cell.x + cell.y, 2.0) < 1.0 ? 1.0 : 0.6;
    }
    const float metallic = clamp(inst.params.x, 0.0, 1.0);
    vec3 color = inst.emissive.rgb * inst.emissive.w;
    const vec3 Ls = -normalize(pc.frame.data.sunDirection.xyz);
    const float NdotL = dot(N, Ls);
    if (NdotL > 0.0)
        color += albedo * (1.0 - metallic) / NOVA_PI * NdotL * pc.frame.data.sunColor.rgb * pc.frame.data.sunDirection.w *
                 nova_traceShadow(P + N * pc.frame.data.shadowBias, Ls, 1e4);
    color += nova_ambient(N, albedo, metallic, pc.frame.data.skyZenith.rgb, pc.frame.data.groundColor.rgb,
                          pc.frame.data.skyZenith.w, 1.0);
    return color;
}

// A direction around the mirror reflection of V, spread by the roughness (a GGX half vector).
vec3 nova_glossyDirection(vec3 N, vec3 V, float roughness, float noise) {
    const vec3 mirror = reflect(-V, N);
    if (roughness < 0.03) return mirror;
    const float a = roughness * roughness, a2 = a * a;
    const float u1 = noise, u2 = fract(noise * 7.13 + 0.37);
    const float cosTheta = sqrt((1.0 - u2) / (1.0 + (a2 - 1.0) * u2)), sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    const float phi = 6.2831853 * u1;
    const vec3 helper = abs(N.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    const vec3 T = normalize(cross(helper, N)), B = cross(N, T);
    const vec3 H = normalize(T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + N * cosTheta);
    const vec3 L = reflect(-V, H);
    return dot(L, N) > 0.0 ? L : mirror;
}

// Ambient occlusion from `rays` short rays (up to `radius`) in the hemisphere: 1 = open.
float nova_rayTracedAo(vec3 origin, vec3 N, float radius, uint rays, float noise) {
    rays = clamp(rays, 1u, 16u);
    const vec3 helper = abs(N.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    const vec3 T = normalize(cross(helper, N)), B = cross(N, T);
    float open = 0.0;
    for (uint i = 0u; i < rays; ++i) {
        const float u1 = fract(noise + float(i) * 0.75487767), u2 = fract(noise * 3.1 + float(i) * 0.56984029);
        const float r = sqrt(u1), phi = 6.2831853 * u2;
        const vec3 dir = T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(max(1.0 - u1, 0.0));
        open += nova_traceShadow(origin, normalize(dir), radius);
    }
    return open / float(rays);
}
#endif

// Shades a surface with the sun, the point lights and the ambient (darkened by `occlusion`);
// `sunShadow` receives the sun visibility (for the Shadows debug view) and `ambientPart` the
// ambient's share of the result (screen-space ambient occlusion darkens that part later).
vec3 nova_shadeParts(vec3 P, vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, float occlusion,
                     out float sunShadow, out vec3 ambientPart) {
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

    // Hemisphere ambient: diffuse, and a rough guess of the specular (what reflections replace).
    const vec3 ambientLight = mix(pc.frame.data.groundColor.rgb, pc.frame.data.skyZenith.rgb, N.y * 0.5 + 0.5) *
                              pc.frame.data.skyZenith.w;
    const vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuseAmbient = ambientLight * albedo * (1.0 - metallic);
    vec3 specularAmbient = ambientLight * F0 * 0.25;
    vec3 reflection = vec3(0.0); // traced: not part of the ambient (screen-space AO leaves it)
#ifdef NOVA_RAY_QUERY
    const vec4 rt = pc.frame.data.rtParams;
    const float noise = nova_noise(gl_FragCoord.xy, rt.w);
    if (rtShadows && (pc.frame.data.flags & FRAME_FLAG_RT_AO) != 0u) {
        const float ao = nova_rayTracedAo(origin, N, rt.x, uint(rt.y), noise);
        diffuseAmbient *= ao;
        specularAmbient *= ao;
    }
    if (rtShadows && (pc.frame.data.flags & FRAME_FLAG_RT_REFLECTIONS) != 0u && roughness < rt.z) {
        // One ray around the mirror direction (spread by the roughness; TAA averages the noise),
        // its light weighted by Fresnel; fading into the guess toward the roughness limit.
        const float fade = 1.0 - smoothstep(rt.z * 0.6, rt.z, roughness);
        const vec3 dir = nova_glossyDirection(N, V, roughness, noise);
        const float NdotV = max(dot(N, V), 1e-4);
        const vec3 Fr = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
        reflection = nova_reflectedRadiance(origin, dir) * Fr * occlusion * fade;
        specularAmbient *= 1.0 - fade;
    }
#endif
    ambientPart = (diffuseAmbient + specularAmbient) * occlusion;
    return color + ambientPart + reflection;
}

// Same, without the ambient's share.
vec3 nova_shadeOccluded(vec3 P, vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, float occlusion,
                        out float sunShadow) {
    vec3 ambientPart;
    return nova_shadeParts(P, N, V, albedo, metallic, roughness, occlusion, sunShadow, ambientPart);
}

// Same, without ambient occlusion.
vec3 nova_shade(vec3 P, vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, out float sunShadow) {
    return nova_shadeOccluded(P, N, V, albedo, metallic, roughness, 1.0, sunShadow);
}

#endif // NOVA_LIGHTING_GLSL
