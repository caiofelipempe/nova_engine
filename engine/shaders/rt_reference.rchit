#version 460
// Closest hit of primary rays (reference view): the surface from its triangle and material (base
// color and its texture, checker, metallic, roughness, emissive), lit by the sun and the point
// lights, each through a shadow ray, plus the hemisphere ambient.
#include "nova/raytracing.glsl"
#include "nova/brdf.glsl"

layout(location = 0) rayPayloadInEXT NovaPrimaryPayload payload;
layout(location = 1) rayPayloadEXT float lightVisible;

hitAttributeEXT vec2 bary;

// 1 if nothing stands between `origin` and `tMax` along `dir`.
float visibility(vec3 origin, vec3 dir, float tMax) {
    lightVisible = 0.0;
    traceRayEXT(accelerationStructureEXT(pc.tlas),
                gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, 0xFF, NOVA_RAY_SHADOW,
                NOVA_RAY_TYPES, NOVA_RAY_SHADOW, origin, 0.0, dir, tMax, 1);
    return lightVisible;
}

void main() {
    const InstanceData inst = pc.instances.items[gl_InstanceCustomIndexEXT];
    const NovaHit hit = nova_hitSurface(inst, gl_PrimitiveID, bary, gl_WorldRayOriginEXT, gl_WorldRayDirectionEXT, gl_HitTEXT);
    const vec3 N = hit.normal, V = -gl_WorldRayDirectionEXT, P = hit.position;

    const NovaHitMaterial mat = nova_hitMaterial(inst, hit, pc.frame.data.originTiled.xyz);
    const vec3 albedo = mat.albedo, emissive = mat.emissive;
    const float metallic = mat.metallic, roughness = mat.roughness;

    const vec3 origin = P + N * pc.bias;
    vec3 color = emissive;
    const vec3 Ls = -normalize(pc.frame.data.sunDirection.xyz);
    if (dot(N, Ls) > 0.0)
        color += nova_brdf(N, V, Ls, albedo, metallic, roughness) * pc.frame.data.sunColor.rgb *
                 pc.frame.data.sunDirection.w * visibility(origin, Ls, 1e5);
    const uint count = min(pc.frame.data.pointLightCount, uint(MAX_POINT_LIGHTS));
    for (uint i = 0u; i < count; ++i) {
        const PointLight light = pc.frame.data.pointLights[i];
        const vec3 toLight = light.positionRadius.xyz - P;
        const float dist = length(toLight), radius = light.positionRadius.w;
        if (dist >= radius) continue;
        const vec3 L = toLight / dist;
        if (dot(N, L) <= 0.0) continue;
        const float window = clamp(1.0 - pow(dist / radius, 4.0), 0.0, 1.0);
        const float atten = window * window / (dist * dist + 1.0);
        const int slot = pc.frame.data.pointShadowSlots[i >> 2u][i & 3u];
        const float shadow = slot == -2 ? 1.0 : visibility(origin, L, max(dist - pc.bias, 0.0));
        color += nova_brdf(N, V, L, albedo, metallic, roughness) * light.colorIntensity.rgb * light.colorIntensity.w * atten * shadow;
    }
    color += nova_ambient(N, albedo, metallic, pc.frame.data.skyZenith.rgb, pc.frame.data.groundColor.rgb,
                          pc.frame.data.skyZenith.w, 1.0);
    payload.color = color;
    payload.hitT = gl_HitTEXT;
}
