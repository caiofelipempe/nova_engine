#version 460
#include "nova/common.glsl"

// Debug lines (nova::DebugDraw): vertices pulled from a per-frame buffer.
struct LineVertex {
    vec3 position;
    uint color; // RGBA8
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LineVertices { LineVertex items[]; };

layout(push_constant) uniform LinePush {
    FrameBuffer frame;
    LineVertices vertices;
} pc;

layout(location = 0) out vec4 vColor;

void main() {
    LineVertex v = pc.vertices.items[gl_VertexIndex];
    vColor = unpackUnorm4x8(v.color);
    gl_Position = pc.frame.data.viewProj * vec4(v.position, 1.0);
    gl_Position.z -= 2e-5 * gl_Position.w; // tiny bias: lines on surfaces win the depth test
}
