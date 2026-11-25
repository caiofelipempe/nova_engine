#version 460
// FXAA (after Timothy Lottes' FXAA 3.11, PC quality): finds the edges of the tonemapped image by
// local contrast in luma, searches along each edge for its ends and blends across it by how far
// the pixel is from the nearer end, plus a sub-pixel blend for single-pixel features. Works on the
// gamma-encoded image (as the tonemap writes it). UVs are clamped by hand: the bindless sampler repeats.
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D g_textures[];

layout(push_constant) uniform FxaaPush {
    uint source;            // the tonemapped image
    float subpix;           // sub-pixel blending (0 off .. 1 soft)
    float edgeThreshold;    // contrast needed, relative to the brightest neighbor
    float edgeThresholdMin; // contrast needed in dark areas (absolute)
    vec2 texel;             // 1 / size
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 fetch(vec2 uv) {
    return textureLod(g_textures[nonuniformEXT(pc.source)], clamp(uv, pc.texel * 0.5, 1.0 - pc.texel * 0.5), 0.0).rgb;
}
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float lumaAt(vec2 uv) { return luma(fetch(uv)); }

const int kSteps = 12;
const float kStep[kSteps] = float[kSteps](1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

void main() {
    vec2 uv = vUV;
    const vec2 t = pc.texel;
    const vec3 rgbM = fetch(uv);
    const float lumaM = luma(rgbM);
    float lumaN = lumaAt(uv + vec2(0.0, -t.y)), lumaS = lumaAt(uv + vec2(0.0, t.y));
    const float lumaE = lumaAt(uv + vec2(t.x, 0.0)), lumaW = lumaAt(uv + vec2(-t.x, 0.0));
    const float maxLuma = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    const float minLuma = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    const float range = maxLuma - minLuma;
    if (range < max(pc.edgeThresholdMin, maxLuma * pc.edgeThreshold)) { // no edge here
        outColor = vec4(rgbM, 1.0);
        return;
    }

    const float lumaNW = lumaAt(uv + vec2(-t.x, -t.y)), lumaNE = lumaAt(uv + vec2(t.x, -t.y));
    const float lumaSW = lumaAt(uv + vec2(-t.x, t.y)), lumaSE = lumaAt(uv + vec2(t.x, t.y));

    // Horizontal or vertical edge: second differences along each axis.
    const float edgeHorz = abs(lumaNW - 2.0 * lumaW + lumaSW) + 2.0 * abs(lumaN - 2.0 * lumaM + lumaS) +
                           abs(lumaNE - 2.0 * lumaE + lumaSE);
    const float edgeVert = abs(lumaNW - 2.0 * lumaN + lumaNE) + 2.0 * abs(lumaW - 2.0 * lumaM + lumaE) +
                           abs(lumaSW - 2.0 * lumaS + lumaSE);
    const bool horzSpan = edgeHorz >= edgeVert;

    // Sub-pixel amount: how much the pixel differs from the average of its neighbors.
    const float average = (2.0 * (lumaN + lumaS + lumaE + lumaW) + (lumaNW + lumaNE + lumaSW + lumaSE)) / 12.0;
    const float subpixC = clamp(abs(average - lumaM) / range, 0.0, 1.0);
    const float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;
    const float subpixH = subpixF * subpixF * pc.subpix;

    // The side of the edge with the larger gradient.
    if (!horzSpan) {
        lumaN = lumaW;
        lumaS = lumaE;
    }
    float lengthSign = horzSpan ? t.y : t.x;
    const float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    const bool pairN = abs(gradientN) >= abs(gradientS);
    const float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    const float lumaPair = (pairN ? lumaN : lumaS) + lumaM; // twice the luma at the edge's middle

    // Search along the edge, both ways, until the luma leaves the edge's.
    vec2 posB = uv;
    if (horzSpan) posB.y += lengthSign * 0.5;
    else posB.x += lengthSign * 0.5;
    const vec2 offset = horzSpan ? vec2(t.x, 0.0) : vec2(0.0, t.y);
    vec2 posN = posB - offset, posP = posB + offset;
    const float gradientScaled = gradient * 0.25;
    float lumaEndN = lumaAt(posN) - lumaPair * 0.5, lumaEndP = lumaAt(posP) - lumaPair * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;
    for (int i = 1; i < kSteps && !(doneN && doneP); ++i) {
        if (!doneN) {
            posN -= offset * kStep[i];
            lumaEndN = lumaAt(posN) - lumaPair * 0.5;
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if (!doneP) {
            posP += offset * kStep[i];
            lumaEndP = lumaAt(posP) - lumaPair * 0.5;
            doneP = abs(lumaEndP) >= gradientScaled;
        }
    }

    // Blend across the edge by how near the pixel is to the end where the edge turns its way.
    const float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
    const float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
    const bool lumaMLessThanEdge = lumaM - lumaPair * 0.5 < 0.0;
    const bool nearerN = dstN < dstP;
    const bool goodSpan = ((nearerN ? lumaEndN : lumaEndP) < 0.0) != lumaMLessThanEdge;
    const float pixelOffset = goodSpan ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0.0;
    const float finalOffset = max(pixelOffset, subpixH);
    if (horzSpan) uv.y += finalOffset * lengthSign;
    else uv.x += finalOffset * lengthSign;
    outColor = vec4(fetch(uv), 1.0);
}
