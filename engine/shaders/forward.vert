#version 460
#include "common.glsl"
#include "forward_push.glsl"

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

void main() {
    InstanceData inst = pc.instances.items[pc.instanceIndex];
    Vertex v = pc.vertices.items[gl_VertexIndex]; // vertex pulling through a buffer address

    vec4 world = inst.model * vec4(v.position, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(inst.normalMatrix) * v.normal;
    vUV = vec2(v.u, v.v);
    gl_Position = pc.frame.data.viewProj * world;
}
