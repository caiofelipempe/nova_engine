#version 460
// Shadow rays that hit nothing: the light is seen.
#include "nova/raytracing.glsl"

layout(location = 1) rayPayloadInEXT float lightVisible;

void main() { lightVisible = 1.0; }
