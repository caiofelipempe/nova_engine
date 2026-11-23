#version 460
// Bloom (after Jimenez, "Next generation post processing in Call of Duty: Advanced Warfare"): a
// chain of half-size images, each a 13-tap filter of the one above (the first a Karis average, so
// single bright pixels do not flicker), then back up, each level adding a 3 × 3 tent of the
// smaller one. UVs are clamped by hand: the bindless sampler repeats.
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D g_textures[];

layout(push_constant) uniform BloomPush {
    uint source;     // the image to filter (down) or this level's downsampled image (up)
    uint lower;      // up: the next smaller level's result
    uint mode;       // 0 first downsample, 1 downsample, 2 upsample
    float threshold; // first downsample: brightness where bloom starts (0 = everything blooms)
    vec2 texel;      // down: 1 / source size; up: 1 / lower size
    float radius;    // up: the tent's size, in texels of the lower level
    float knee;      // first downsample: softness of the threshold
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 fetch(uint tex, vec2 uv, vec2 texel) {
    return textureLod(g_textures[nonuniformEXT(tex)], clamp(uv, texel * 0.5, 1.0 - texel * 0.5), 0.0).rgb;
}

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
// Karis average: each group weighted down by its brightness.
float karis(vec3 c) { return 1.0 / (1.0 + luma(c)); }

vec3 downsample(bool first) {
    const vec2 t = pc.texel;
    const vec3 a = fetch(pc.source, vUV + t * vec2(-2, 2), t), b = fetch(pc.source, vUV + t * vec2(0, 2), t);
    const vec3 c = fetch(pc.source, vUV + t * vec2(2, 2), t), d = fetch(pc.source, vUV + t * vec2(-2, 0), t);
    const vec3 e = fetch(pc.source, vUV, t), f = fetch(pc.source, vUV + t * vec2(2, 0), t);
    const vec3 g = fetch(pc.source, vUV + t * vec2(-2, -2), t), h = fetch(pc.source, vUV + t * vec2(0, -2), t);
    const vec3 i = fetch(pc.source, vUV + t * vec2(2, -2), t), j = fetch(pc.source, vUV + t * vec2(-1, 1), t);
    const vec3 k = fetch(pc.source, vUV + t * vec2(1, 1), t), l = fetch(pc.source, vUV + t * vec2(-1, -1), t);
    const vec3 m = fetch(pc.source, vUV + t * vec2(1, -1), t);
    if (!first) return e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    // Five overlapping groups (the inner four samples, and four 2 × 2 corners), Karis-weighted.
    const vec3 g0 = (j + k + l + m) * 0.25, g1 = (a + b + d + e) * 0.25, g2 = (b + c + e + f) * 0.25;
    const vec3 g3 = (d + e + g + h) * 0.25, g4 = (e + f + h + i) * 0.25;
    const float w0 = 0.5 * karis(g0), w1 = 0.125 * karis(g1), w2 = 0.125 * karis(g2), w3 = 0.125 * karis(g3),
                w4 = 0.125 * karis(g4);
    vec3 color = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
    if (pc.threshold > 0.0) { // soft knee: only what is brighter than the threshold blooms
        const float brightness = max(color.r, max(color.g, color.b));
        float soft = clamp(brightness - pc.threshold + pc.knee, 0.0, 2.0 * pc.knee);
        soft = soft * soft / (4.0 * pc.knee + 1e-5);
        color *= max(soft, brightness - pc.threshold) / max(brightness, 1e-5);
    }
    return color;
}

vec3 upsample() {
    const vec2 t = pc.texel * pc.radius;
    vec3 sum = fetch(pc.lower, vUV, pc.texel) * 4.0;
    sum += (fetch(pc.lower, vUV + vec2(-t.x, 0), pc.texel) + fetch(pc.lower, vUV + vec2(t.x, 0), pc.texel) +
            fetch(pc.lower, vUV + vec2(0, -t.y), pc.texel) + fetch(pc.lower, vUV + vec2(0, t.y), pc.texel)) * 2.0;
    sum += fetch(pc.lower, vUV + vec2(-t.x, -t.y), pc.texel) + fetch(pc.lower, vUV + vec2(t.x, -t.y), pc.texel) +
           fetch(pc.lower, vUV + vec2(-t.x, t.y), pc.texel) + fetch(pc.lower, vUV + vec2(t.x, t.y), pc.texel);
    const vec2 own = 1.0 / vec2(textureSize(g_textures[nonuniformEXT(pc.source)], 0));
    return fetch(pc.source, vUV, own) + sum / 16.0;
}

void main() {
    outColor = vec4(pc.mode == 2u ? upsample() : downsample(pc.mode == 0u), 1.0);
}
