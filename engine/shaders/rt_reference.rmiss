#version 460
// Primary rays that hit nothing: the sky.
#include "nova/raytracing.glsl"
#include "nova/sky.glsl"

layout(location = 0) rayPayloadInEXT NovaPrimaryPayload payload;

void main() {
    payload.color = nova_skyColor(pc.frame, normalize(gl_WorldRayDirectionEXT));
    payload.hitT = -1.0;
}
