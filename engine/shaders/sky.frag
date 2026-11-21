#version 460
#include "nova/common.glsl"

layout(push_constant) uniform SkyPush { FrameBuffer frame; } pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

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
}
