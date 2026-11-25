#version 460
// Temporal anti-aliasing resolve. Each frame the projection is offset by a different sub-pixel
// amount (FrameData::jitter); this pass blends the frame into the history of earlier ones:
//  - the history is read where the pixel was last frame (its motion vector, taken from the nearest
//    surface of the 3 × 3 neighborhood so edges of moving things carry their motion), with a
//    Catmull-Rom filter (sharper than bilinear);
//  - it is clipped to the color range of this frame's 3 × 3 neighborhood (mean ± standard
//    deviation, in YCoCg), which rejects what is no longer there (ghosts, disocclusion);
//  - colors are blended after a reversible tonemap (c / (1 + luma)), so bright pixels do not flicker.
// UVs are clamped by hand: the bindless sampler repeats.
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D g_textures[];

layout(push_constant) uniform TaaPush {
    uint color;
    uint velocity;
    uint depth;
    uint history;
    vec2 texel;
    float feedback; // weight of this frame (the rest is history)
    uint mode;      // 0 resolve, 1 no history, 2 show the motion
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 current(ivec2 p) {
    const ivec2 size = ivec2(round(1.0 / pc.texel));
    return max(texelFetch(g_textures[nonuniformEXT(pc.color)], clamp(p, ivec2(0), size - 1), 0).rgb, vec3(0.0));
}

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 compress(vec3 c) { return c / (1.0 + luma(c)); }
vec3 uncompress(vec3 c) { return c / max(1.0 - luma(c), 1e-4); }

vec3 toYCoCg(vec3 c) {
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}
vec3 fromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

vec3 history(vec2 uv) {
    return max(textureLod(g_textures[nonuniformEXT(pc.history)], clamp(uv, pc.texel * 0.5, 1.0 - pc.texel * 0.5), 0.0).rgb,
               vec3(0.0));
}

// Catmull-Rom from 9 bilinear reads (5 with the corners dropped), after Jimenez / MJP.
vec3 historyCatmullRom(vec2 uv) {
    const vec2 size = 1.0 / pc.texel;
    const vec2 samplePos = uv * size;
    const vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    const vec2 f = samplePos - texPos1;
    const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const vec2 w3 = f * f * (-0.5 + 0.5 * f);
    const vec2 w12 = w1 + w2;
    const vec2 offset12 = w2 / w12;
    const vec2 texPos0 = (texPos1 - 1.0) * pc.texel;
    const vec2 texPos3 = (texPos1 + 2.0) * pc.texel;
    const vec2 texPos12 = (texPos1 + offset12) * pc.texel;
    vec3 result = history(vec2(texPos12.x, texPos0.y)) * w12.x * w0.y;
    result += history(vec2(texPos0.x, texPos12.y)) * w0.x * w12.y;
    result += history(vec2(texPos12.x, texPos12.y)) * w12.x * w12.y;
    result += history(vec2(texPos3.x, texPos12.y)) * w3.x * w12.y;
    result += history(vec2(texPos12.x, texPos3.y)) * w12.x * w3.y;
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(result / weight, vec3(0.0));
}

// The point of the segment from the box's center to `c` where it leaves the box (or `c`).
vec3 clipToBox(vec3 boxMin, vec3 boxMax, vec3 c) {
    const vec3 center = 0.5 * (boxMax + boxMin), extents = 0.5 * (boxMax - boxMin) + 1e-5;
    const vec3 d = c - center;
    const vec3 units = abs(d / extents);
    const float most = max(units.x, max(units.y, units.z));
    return most > 1.0 ? center + d / most : c;
}

void main() {
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const ivec2 size = ivec2(round(1.0 / pc.texel));

    // The motion of the nearest surface around the pixel.
    float nearest = 2.0;
    ivec2 nearestPixel = pixel;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
            const float d = texelFetch(g_textures[nonuniformEXT(pc.depth)], q, 0).r;
            if (d < nearest) {
                nearest = d;
                nearestPixel = q;
            }
        }
    const vec2 motion = texelFetch(g_textures[nonuniformEXT(pc.velocity)], nearestPixel, 0).rg;
    if (pc.mode == 2u) { // DebugView::Motion: direction as hue, 10 pixels of motion = full color
        const vec2 m = motion / pc.texel * 0.1;
        outColor = vec4(clamp(vec3(0.5 + m.x, 0.5 + m.y, 0.5 - 0.5 * (m.x + m.y)), 0.0, 1.0) * min(length(m) * 4.0, 1.0), 1.0);
        return;
    }

    const vec3 centerColor = current(pixel);
    const vec2 prevUV = vUV - motion;
    if (pc.mode == 1u || any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0)))) {
        outColor = vec4(centerColor, 1.0); // nothing to blend with
        return;
    }

    // This frame's neighborhood: mean and standard deviation (YCoCg of the compressed colors).
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const vec3 c = toYCoCg(compress(current(pixel + ivec2(x, y))));
            m1 += c;
            m2 += c * c;
        }
    const vec3 mean = m1 / 9.0;
    const vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    const float gamma = 1.25;
    const vec3 boxMin = mean - gamma * sigma, boxMax = mean + gamma * sigma;

    const vec3 previous = clipToBox(boxMin, boxMax, toYCoCg(compress(historyCatmullRom(prevUV))));
    const vec3 now = toYCoCg(compress(centerColor));
    // Faster motion: the history is blurrier (resampled), so this frame weighs a little more.
    const float speed = length(motion / pc.texel);
    const float feedback = clamp(mix(pc.feedback, pc.feedback * 2.5, clamp(speed / 8.0, 0.0, 1.0)), 0.0, 1.0);
    const vec3 blended = fromYCoCg(mix(previous, now, feedback));
    outColor = vec4(max(uncompress(blended), vec3(0.0)), 1.0);
}
