#version 460
// Distance fog, a post-process effect reading depth: the image fades toward the horizon's color
// with the distance from the camera (nova_linearDepth); the sky itself stays.
// PostProcessDesc::params: x density (1/m), y distance where it starts (m), z most fog (0..1).
#include "nova/postprocess.glsl"

vec3 postProcess(vec2 uv) {
    const vec3 color = nova_scene(uv);
    if (nova_depth(uv) >= 1.0) return color; // nothing drawn here: the sky
    const vec4 p = nova_params();
    const float distance = max(nova_linearDepth(uv) - p.y, 0.0);
    const float fog = (1.0 - exp(-distance * p.x)) * p.z;
    return mix(color, nova_frame().skyHorizon.rgb, fog);
}
