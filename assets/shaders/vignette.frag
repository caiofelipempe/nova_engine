#version 460
// Sample post effect: vignette + a touch of chromatic aberration at the edges.
// PostProcessDesc::params: x = vignette strength (0..1), y = chromatic aberration in pixels.
#include "nova/postprocess.glsl"

vec3 postProcess(vec2 uv) {
    vec2 fromCenter = uv - 0.5;
    vec2 shift = fromCenter * nova_params().y * nova_texelSize() * 2.0;
    vec3 c = vec3(nova_scene(uv + shift).r, nova_scene(uv).g, nova_scene(uv - shift).b);

    float d = length(fromCenter * vec2(1.0, 0.8));
    float vignette = smoothstep(0.85, 0.25, d);
    return c * mix(1.0, vignette, clamp(nova_params().x, 0.0, 1.0));
}
