#version 460
// Path tracer, miss: nothing hit (the ray generation shader adds the sky).
#include "nova/raytracing.glsl"

layout(location = 0) rayPayloadInEXT NovaSurfacePayload surface;

void main() { surface.hitT = -1.0; }
