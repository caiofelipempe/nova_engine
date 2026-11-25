#version 460
#include "nova/common.glsl"
#include "nova/sky.glsl"

layout(push_constant) uniform SkyPush { FrameBuffer frame; } pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outAmbient; // none: the sky is not occluded (forward pass's second output)
layout(location = 2) out vec4 outMotion;  // the sky moves only as the camera turns (TAA)

void main() {
    vec2 ndc = vUV * 2.0 - 1.0;
    vec4 p = pc.frame.data.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(p.xyz / p.w - pc.frame.data.cameraPos.xyz);

    outColor = vec4(nova_skyColor(pc.frame, dir), 1.0);
    outAmbient = vec4(0.0);
    // A direction (w = 0): where it was on screen last frame depends only on how the camera turned.
    const vec4 prevClip = pc.frame.data.prevViewProj * vec4(dir, 0.0);
    outMotion = vec4(nova_motion(vec4(ndc, 1.0, 1.0), prevClip, pc.frame.data.jitter.xy), 0.0, 1.0);
}
