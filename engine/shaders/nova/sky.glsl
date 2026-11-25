// The engine's sky: a gradient from the horizon up to the zenith and down to the ground, with the
// sun's disc and glow. Used by sky.frag and by rays that miss everything. Public.
#ifndef NOVA_SKY_GLSL
#define NOVA_SKY_GLSL
#include "nova/common.glsl"

// Radiance seen along `dir` (unit), from the frame's sky colors and sun; `sunDisc` false leaves
// out the sun's disc (rays that bounced, when the sun is sampled directly).
vec3 nova_skyRadiance(FrameBuffer frame, vec3 dir, bool sunDisc) {
    vec3 zenith = frame.data.skyZenith.rgb;
    vec3 horizon = frame.data.skyHorizon.rgb;
    vec3 ground = frame.data.groundColor.rgb;
    vec3 color = dir.y >= 0.0 ? mix(horizon, zenith, pow(dir.y, 0.5))
                              : mix(horizon, ground, pow(-dir.y, 0.35));
    vec3 L = -normalize(frame.data.sunDirection.xyz);
    float s = max(dot(dir, L), 0.0);
    vec3 sun = frame.data.sunColor.rgb * frame.data.sunDirection.w;
    color += sun * ((sunDisc ? smoothstep(0.9994, 0.9997, s) * 8.0 : 0.0) + pow(s, 96.0) * 0.15);
    return color;
}

vec3 nova_skyColor(FrameBuffer frame, vec3 dir) { return nova_skyRadiance(frame, dir, true); }

#endif // NOVA_SKY_GLSL
