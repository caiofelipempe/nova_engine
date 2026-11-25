#version 460
// Path tracer, closest hit: reports the surface (position, normal facing the ray, material); the
// ray generation shader does the shading and picks the next direction.
#include "nova/raytracing.glsl"

layout(location = 0) rayPayloadInEXT NovaSurfacePayload surface;

hitAttributeEXT vec2 bary;

void main() {
    const InstanceData inst = pc.instances.items[gl_InstanceCustomIndexEXT];
    const NovaHit hit = nova_hitSurface(inst, gl_PrimitiveID, bary, gl_WorldRayOriginEXT, gl_WorldRayDirectionEXT, gl_HitTEXT);
    const NovaHitMaterial mat = nova_hitMaterial(inst, hit, pc.frame.data.originTiled.xyz);
    surface.position = hit.position;
    surface.hitT = gl_HitTEXT;
    surface.normal = hit.normal;
    surface.albedo = mat.albedo;
    surface.metallic = mat.metallic;
    surface.roughness = mat.roughness;
    surface.emissive = mat.emissive;
}
