#version 460
#ifdef NOVA_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif
#include "common.glsl"
#include "forward_push.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float D_GGX(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float G_SchlickGGX(float NdotX, float k) { return NdotX / (NdotX * (1.0 - k) + k); }

vec3 F_Schlick(float VdotH, vec3 F0) { return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0); }

// Cook-Torrance GGX, returns radiance factor already multiplied by N.L
vec3 brdf(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float a = max(roughness * roughness, 0.002);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = F_Schlick(VdotH, F0);
    float D = D_GGX(NdotH, a);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float G = G_SchlickGGX(NdotV, k) * G_SchlickGGX(NdotL, k);

    vec3 specular = D * G * F / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / PI + specular) * NdotL;
}

// 1 = lit, 0 = occluded. Without ray query support this is a no-op (future: shadow maps).
float traceShadow(vec3 origin, vec3 dir, float tMax) {
#ifdef NOVA_RAY_QUERY
    if (pc.tlas == 0ul) return 1.0;
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, accelerationStructureEXT(pc.tlas),
                          gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF,
                          origin, 0.0, dir, tMax);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
#else
    return 1.0;
#endif
}

void main() {
    InstanceData inst = pc.instances.items[pc.instanceIndex];
    uint matFlags = floatBitsToUint(inst.params.z);

    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    vec3 camPos = pc.frame.data.cameraPos.xyz;
    vec3 V = normalize(camPos - vWorldPos);

    vec3 albedo = inst.baseColor.rgb;
    int texIndex = int(inst.params.w);
    if (texIndex >= 0) albedo *= texture(g_textures[nonuniformEXT(texIndex)], vUV).rgb;
    if ((matFlags & MATERIAL_FLAG_CHECKER) != 0u) {
        vec2 c = floor(vWorldPos.xz);
        albedo *= mod(c.x + c.y, 2.0) < 1.0 ? 1.0 : 0.6;
    }
    float metallic = clamp(inst.params.x, 0.0, 1.0);
    float roughness = clamp(inst.params.y, 0.04, 1.0);

    bool rtShadows = (pc.frame.data.flags & FRAME_FLAG_RT_SHADOWS) != 0u;
    float bias = pc.frame.data.shadowBias;
    vec3 origin = vWorldPos + N * bias;

    vec3 color = vec3(0.0);
    float sunShadow = 1.0;

    // Directional light (sun)
    vec3 Ls = -normalize(pc.frame.data.sunDirection.xyz);
    if (dot(N, Ls) > 0.0) {
        if (rtShadows) sunShadow = traceShadow(origin, Ls, 10000.0);
        color += brdf(N, V, Ls, albedo, metallic, roughness) * pc.frame.data.sunColor.rgb *
                 pc.frame.data.sunDirection.w * sunShadow;
    }

    // Point lights
    uint count = min(pc.frame.data.pointLightCount, uint(MAX_POINT_LIGHTS));
    for (uint i = 0u; i < count; ++i) {
        PointLight light = pc.frame.data.pointLights[i];
        vec3 toLight = light.positionRadius.xyz - vWorldPos;
        float dist = length(toLight);
        float radius = light.positionRadius.w;
        if (dist >= radius) continue;
        vec3 L = toLight / dist;
        if (dot(N, L) <= 0.0) continue;
        float window = clamp(1.0 - pow(dist / radius, 4.0), 0.0, 1.0);
        float atten = window * window / (dist * dist + 1.0);
        float shadow = rtShadows ? traceShadow(origin, L, max(dist - bias, 0.0)) : 1.0;
        color += brdf(N, V, L, albedo, metallic, roughness) * light.colorIntensity.rgb *
                 light.colorIntensity.w * atten * shadow;
    }

    // Hemisphere ambient
    vec3 sky = pc.frame.data.skyZenith.rgb;
    vec3 ground = pc.frame.data.groundColor.rgb;
    vec3 ambient = mix(ground, sky, N.y * 0.5 + 0.5) * pc.frame.data.skyZenith.w;
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    color += ambient * (albedo * (1.0 - metallic) + F0 * 0.25);

    color += inst.emissive.rgb * inst.emissive.w;

    switch (pc.debugView) {
        case 1u: color = N * 0.5 + 0.5; break;
        case 2u: color = vec3(sunShadow); break;
        case 3u: color = albedo; break;
        default: break;
    }
    outColor = vec4(color, 1.0);
}
