#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D g_textures[];

layout(push_constant) uniform TonemapPush {
    uint hdrTexture;
    float exposure;
    uint tonemapper; // 0 ACES (fitted), 1 Reinhard, 2 none
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 acesFitted(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(g_textures[nonuniformEXT(pc.hdrTexture)], vUV).rgb * pc.exposure;
    vec3 c;
    if (pc.tonemapper == 0u) c = acesFitted(hdr);
    else if (pc.tonemapper == 1u) c = hdr / (1.0 + hdr);
    else c = clamp(hdr, 0.0, 1.0);
    c = pow(c, vec3(1.0 / 2.2)); // output target is UNORM: encode gamma here
    outColor = vec4(c, 1.0);
}
