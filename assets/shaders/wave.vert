#version 460
// A material vertex shader: a flag in the wind. The mesh (a plane on XZ, normal +Y, as
// MeshData::plane) ripples along its normal in waves that travel along X, still at the edge x = -0.5
// (the pole) and freest at the other. Material::customParams: x amplitude, y wavelength (object
// units), z speed (waves per second); zeros pick the defaults.
#include "nova/vertex.glsl"

void vertex(inout NovaVertex v) {
    const vec4 p = nova_custom(v.instance);
    const float amplitude = p.x > 0.0 ? p.x : 0.3;
    const float wavelength = p.y > 0.0 ? p.y : 0.5;
    const float speed = p.z > 0.0 ? p.z : 1.5;
    const float k = 6.2831853 / wavelength;
    const float phase = v.position.x * k - nova_time() * speed * 6.2831853;
    const float free = clamp(v.position.x + 0.5, 0.0, 1.0); // 0 at the pole, 1 at the far edge
    v.position.y += sin(phase) * amplitude * free;
    // The normal tilted by the slope of the surface along X (d(height)/dx).
    const float slope = cos(phase) * amplitude * free * k + sin(phase) * amplitude;
    v.normal = normalize(vec3(-slope, 1.0, 0.0));
}
