#version 460
#include "nova/common.glsl"

layout(push_constant) uniform SkyPush { FrameBuffer frame; } pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outAmbient; // none: the sky is not occluded (forward pass's second output)
layout(location = 2) out vec4 outMotion;  // the sky moves only as the camera turns (TAA)

void main() {
    vec2 ndc = vUV * 2.0 - 1.0;
    vec4 p = pc.frame.data.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(p.xyz / p.w - pc.frame.data.cameraPos.xyz);

    vec3 zenith = pc.frame.data.skyZenith.rgb;
    vec3 horizon = pc.frame.data.skyHorizon.rgb;
    vec3 ground = pc.frame.data.groundColor.rgb;

    vec3 color = dir.y >= 0.0 ? mix(horizon, zenith, pow(dir.y, 0.5))
                              : mix(horizon, ground, pow(-dir.y, 0.35));

    vec3 L = -normalize(pc.frame.data.sunDirection.xyz);
    float s = max(dot(dir, L), 0.0);
    vec3 sun = pc.frame.data.sunColor.rgb * pc.frame.data.sunDirection.w;
    color += sun * (smoothstep(0.9994, 0.9997, s) * 8.0 + pow(s, 96.0) * 0.15);

    outColor = vec4(color, 1.0);
    outAmbient = vec4(0.0);
    // A direction (w = 0): where it was on screen last frame depends only on how the camera turned.
    const vec4 prevClip = pc.frame.data.prevViewProj * vec4(dir, 0.0);
    outMotion = vec4(nova_motion(vec4(ndc, 1.0, 1.0), prevClip, pc.frame.data.jitter.xy), 0.0, 1.0);
}
