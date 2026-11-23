#version 460
#include "nova/common.glsl"
#include "nova/forward_push.glsl"

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

#if NOVA_WRAP_RENDER_MODES
// Objects across a seam of the circular world (RenderSettings::wrapShift). Positions are camera
// relative; `cell` is the center of the lap this copy is drawn in (0 for the lap around the camera).
#define NOVA_WRAP_TRIANGLE 0u
#define NOVA_WRAP_OBJECT 1u
#define NOVA_WRAP_SPLIT 2u
#define NOVA_WRAP_CROP 3u

out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[3];
};

// Whole laps that bring `d` (from the lap's center) within half a lap, on the circular axes.
vec3 lapShift(vec3 d, vec3 size) {
    vec3 laps = floor(d / max(size, vec3(1e-6)) + 0.5);
    return mix(vec3(0.0), -laps * size, greaterThan(size, vec3(0.0)));
}
#endif

void main() {
    InstanceData inst = pc.instances.items[pc.instanceIndex];
#if NOVA_WRAP_RENDER_MODES
    const vec3 size = pc.frame.data.wrapSize.xyz;
    const uint mode = uint(pc.frame.data.wrapSize.w);
    const vec3 cell = inst.extra.yzw;
    Vertex v;
    vec4 world;
    if (uint64_t(pc.indices) != 0ul) {
        // Non-indexed draw of an object across a seam: the whole triangle goes to the lap its
        // centroid is in, so the object is drawn partly on each side.
        const uint triangle = uint(gl_VertexIndex) / 3u, corner = uint(gl_VertexIndex) % 3u;
        vec3 p[3];
        for (uint k = 0u; k < 3u; ++k) {
            Vertex vk = pc.vertices.items[pc.indices.items[triangle * 3u + k]];
            p[k] = (inst.model * vec4(vk.position, 1.0)).xyz;
            if (k == corner) v = vk;
        }
        const vec3 middle = (p[0] + p[1] + p[2]) / 3.0; // the centroid
        world = vec4(p[corner] + lapShift(middle - cell, size), 1.0);
    } else {
        v = pc.vertices.items[gl_VertexIndex];
        world = inst.model * vec4(v.position, 1.0);
    }
    // Split and crop: every copy is cut at the edges of its lap (the planes half a lap from its center).
    const bool clipped = mode == NOVA_WRAP_SPLIT || mode == NOVA_WRAP_CROP;
    for (int i = 0; i < 3; ++i)
        gl_ClipDistance[i] = (clipped && size[i] > 0.0) ? size[i] * 0.5 - abs(world[i] - cell[i]) : 1.0;
#else
    Vertex v = pc.vertices.items[gl_VertexIndex]; // vertex pulling through a buffer address
    vec4 world = inst.model * vec4(v.position, 1.0);
#endif
    vWorldPos = world.xyz;
    vNormal = mat3(inst.normalMatrix) * v.normal;
    vUV = vec2(v.u, v.v);
    gl_Position = pc.frame.data.viewProj * world;
}
