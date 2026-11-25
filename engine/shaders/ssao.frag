#version 460
// Screen-space ambient occlusion, from the depth buffer alone (the forward renderer keeps no
// normals): positions and normals are rebuilt from depth.
//  mode 0: the occlusion (R8): 16 samples in the normal's hemisphere, within `radius` (m),
//          rotated per pixel (interleaved gradient noise); 1 = open.
//  mode 1: applies it (HDR): the occlusion blurred over 5 × 5 pixels of similar depth, then the
//          ambient light's share of the image (the forward pass's second output) darkened by it.
//  mode 2: the blurred occlusion as gray (DebugView::AmbientOcclusion).
#include "nova/common.glsl"

layout(push_constant) uniform SsaoPush {
    FrameBuffer frame;
    uint depthTexture;
    uint mode;
    uint colorTexture;   // modes 1, 2: the HDR image
    uint ambientTexture; // modes 1, 2: the ambient's share of it
    uint aoTexture;      // modes 1, 2: the occlusion
    uint _pad0;
    vec2 texel;          // 1 / the image's size
    float radius;        // sampling radius (m)
    float intensity;     // the occlusion raised to this power
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float depthAt(ivec2 p) {
    const ivec2 size = ivec2(round(1.0 / pc.texel));
    return texelFetch(g_textures[nonuniformEXT(pc.depthTexture)], clamp(p, ivec2(0), size - 1), 0).r;
}

// View-space position of a point of the screen at a depth (a perspective projection, depth 0..1).
vec3 viewPosition(vec2 uv, float depth) {
    const mat4 P = pc.frame.data.proj;
    const float z = -P[3][2] / (depth + P[2][2]);
    const vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * -z / P[0][0], ndc.y * -z / P[1][1], z);
}

vec3 positionAt(ivec2 p) { return viewPosition((vec2(p) + 0.5) * pc.texel, depthAt(p)); }

float occlusion(ivec2 pixel) {
    const float depth = depthAt(pixel);
    if (depth >= 1.0) return 1.0; // the sky
    const vec3 P = positionAt(pixel);
    // The normal from the neighbors, on each axis the side nearer in depth (no smearing across edges).
    const vec3 l = positionAt(pixel - ivec2(1, 0)), r = positionAt(pixel + ivec2(1, 0));
    const vec3 u = positionAt(pixel - ivec2(0, 1)), d = positionAt(pixel + ivec2(0, 1));
    const vec3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;
    const vec3 dy = abs(d.z - P.z) < abs(P.z - u.z) ? d - P : P - u;
    vec3 N = normalize(cross(dx, dy));
    if (dot(N, P) > 0.0) N = -N; // facing the camera

    const float noise = fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715))));
    const vec3 helper = abs(N.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    const vec3 T = normalize(cross(helper, N)), B = cross(N, T);
    const mat4 proj = pc.frame.data.proj;
    const float bias = 0.02 * pc.radius + 0.001 * -P.z;
    const int kSamples = 16;
    float occluded = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        // Cosine-weighted directions on a spiral, turned by the noise; nearer samples weigh more.
        const float t = (float(i) + 0.5) / float(kSamples);
        const float phi = float(i) * 2.39996323 + noise * 6.2831853;
        const float sinTheta = sqrt(t);
        const vec3 dir = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, sqrt(1.0 - t));
        const float scale = mix(0.1, 1.0, fract(float(i) * 0.618034 + noise) * fract(float(i) * 0.618034 + noise));
        const vec3 S = P + (T * dir.x + B * dir.y + N * dir.z) * pc.radius * scale;
        const vec4 clip = proj * vec4(S, 1.0);
        if (clip.w <= 0.0) continue;
        const vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) continue;
        const float sceneDepth = depthAt(ivec2(uv / pc.texel));
        if (sceneDepth >= 1.0) continue;
        const float sceneZ = viewPosition(uv, sceneDepth).z;
        const float range = smoothstep(0.0, 1.0, pc.radius / max(abs(P.z - sceneZ), 1e-4)); // far behind: not this
        occluded += (sceneZ >= S.z + bias ? 1.0 : 0.0) * range;
    }
    return pow(clamp(1.0 - occluded / float(kSamples), 0.0, 1.0), pc.intensity);
}

// The occlusion over 5 × 5 pixels, each weighted by how near its depth is to this one's.
float blurredOcclusion(ivec2 pixel) {
    const float depth = depthAt(pixel);
    if (depth >= 1.0) return 1.0;
    const float z = viewPosition((vec2(pixel) + 0.5) * pc.texel, depth).z;
    float sum = 0.0, weight = 0.0;
    const ivec2 size = ivec2(round(1.0 / pc.texel));
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            const ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
            const float qz = viewPosition((vec2(q) + 0.5) * pc.texel, depthAt(q)).z;
            const float w = exp(-abs(qz - z) / (0.02 * abs(z) + 0.01));
            sum += texelFetch(g_textures[nonuniformEXT(pc.aoTexture)], q, 0).r * w;
            weight += w;
        }
    return sum / max(weight, 1e-4);
}

void main() {
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (pc.mode == 0u) {
        outColor = vec4(occlusion(pixel), 0.0, 0.0, 1.0);
        return;
    }
    const float ao = blurredOcclusion(pixel);
    if (pc.mode == 2u) {
        outColor = vec4(vec3(ao), 1.0);
        return;
    }
    const vec3 color = texelFetch(g_textures[nonuniformEXT(pc.colorTexture)], pixel, 0).rgb;
    const vec3 ambient = texelFetch(g_textures[nonuniformEXT(pc.ambientTexture)], pixel, 0).rgb;
    outColor = vec4(max(color - ambient * (1.0 - ao), vec3(0.0)), 1.0);
}
