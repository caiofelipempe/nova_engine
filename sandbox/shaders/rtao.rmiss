#version 460
// Sandbox RT AO: the ray hit nothing within the radius.
#include "nova/raytracing.glsl"

layout(location = 0) rayPayloadInEXT float open;

void main() { open = 1.0; }
