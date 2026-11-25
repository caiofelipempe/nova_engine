#version 460
#include "nova/common.glsl"
#include "nova/forward_push.glsl"

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) flat out uint vInstance; // index into the frame's instances (nova_instance())
layout(location = 4) out vec4 vClip;          // clip position (jittered, as rasterized): motion vectors
layout(location = 5) out vec4 vPrevClip;      // the same point last frame (InstanceData::prevModel, FrameData::prevViewProj)

#if NOVA_WRAP_RENDER_MODES
// Objects across a seam of the circular world (RenderSettings::wrapShift). Positions are camera
// relative; `cell` is the center of the lap this copy is drawn in (0 for the lap around the camera).
#define NOVA_WRAP_TRIANGLE 0u
#define NOVA_WRAP_OBJECT 1u
#define NOVA_WRAP_SPLIT 2u
#define NOVA_WRAP_CROP 3u

out gl_PerVertex {
    invariant vec4 gl_Position; // the depth prepass and the forward pass must agree bit for bit
    float gl_ClipDistance[3];
};

// Whole laps that bring `d` (from the lap's center) within half a lap, on the circular axes.
vec3 lapShift(vec3 d, vec3 size) {
    vec3 laps = floor(d / max(size, vec3(1e-6)) + 0.5);
    return mix(vec3(0.0), -laps * size, greaterThan(size, vec3(0.0)));
}
#else
invariant gl_Position; // the depth prepass and the forward pass must agree bit for bit
#endif

// Skinned meshes: the vertex in the bind pose, moved by its joints (mesh space).
Vertex nova_skinned(uint index, Vertex v) {
    if (uint64_t(pc.skin) == 0ul) return v;
    const SkinVertex s = pc.skin.items[index];
    const uint j[4] = uint[4](s.joints.x & 0xFFFFu, s.joints.x >> 16, s.joints.y & 0xFFFFu, s.joints.y >> 16);
    mat4 m = mat4(0.0);
    for (int k = 0; k < 4; ++k) m += s.weights[k] * pc.joints.items[j[k]];
    v.position = (m * vec4(v.position, 1.0)).xyz;
    v.normal = mat3(m) * v.normal;
    return v;
}

// A skinned vertex's position last frame (pc.prevJoints), from its bind pose; `now` when there is none.
vec3 nova_skinnedPrevious(uint index, vec3 bindPosition, vec3 now) {
    if (uint64_t(pc.skin) == 0ul || uint64_t(pc.prevJoints) == 0ul) return now;
    const SkinVertex s = pc.skin.items[index];
    const uint j[4] = uint[4](s.joints.x & 0xFFFFu, s.joints.x >> 16, s.joints.y & 0xFFFFu, s.joints.y >> 16);
    mat4 m = mat4(0.0);
    for (int k = 0; k < 4; ++k) m += s.weights[k] * pc.prevJoints.items[j[k]];
    return (m * vec4(bindPosition, 1.0)).xyz;
}

void main() {
    // A draw of one instance, or of a group culled on the GPU (its visible instances, in a list).
    const uint instanceIndex = uint64_t(pc.visible) != 0ul ? pc.visible.items[gl_InstanceIndex] : pc.instanceIndex;
    vInstance = instanceIndex;
    InstanceData inst = pc.instances.items[instanceIndex];
#if NOVA_WRAP_RENDER_MODES
    const vec3 size = pc.frame.data.wrapSize.xyz;
    const uint mode = uint(pc.frame.data.wrapSize.w);
    const vec3 cell = inst.extra.yzw;
    Vertex v;
    vec4 world, prevWorld;
    if (uint64_t(pc.indices) != 0ul) {
        // Non-indexed draw of an object across a seam: the whole triangle goes to the lap its
        // centroid is in, so the object is drawn partly on each side.
        const uint triangle = uint(gl_VertexIndex) / 3u, corner = uint(gl_VertexIndex) % 3u;
        vec3 p[3];
        vec3 prevCorner = vec3(0.0);
        for (uint k = 0u; k < 3u; ++k) {
            const uint index = pc.indices.items[triangle * 3u + k];
            const Vertex bind = pc.vertices.items[index];
            Vertex vk = nova_skinned(index, bind);
            p[k] = (inst.model * vec4(vk.position, 1.0)).xyz;
            if (k == corner) {
                v = vk;
                prevCorner = (inst.prevModel * vec4(nova_skinnedPrevious(index, bind.position, vk.position), 1.0)).xyz;
            }
        }
        const vec3 middle = (p[0] + p[1] + p[2]) / 3.0; // the centroid
        const vec3 shift = lapShift(middle - cell, size);
        world = vec4(p[corner] + shift, 1.0);
        prevWorld = vec4(prevCorner + shift, 1.0); // the same lap as now
    } else {
        const Vertex bind = pc.vertices.items[gl_VertexIndex];
        v = nova_skinned(gl_VertexIndex, bind);
        world = inst.model * vec4(v.position, 1.0);
        prevWorld = inst.prevModel * vec4(nova_skinnedPrevious(gl_VertexIndex, bind.position, v.position), 1.0);
    }
    // Split and crop: every copy is cut at the edges of its lap (the planes half a lap from its center).
    const bool clipped = mode == NOVA_WRAP_SPLIT || mode == NOVA_WRAP_CROP;
    for (int i = 0; i < 3; ++i)
        gl_ClipDistance[i] = (clipped && size[i] > 0.0) ? size[i] * 0.5 - abs(world[i] - cell[i]) : 1.0;
#else
    const Vertex bind = pc.vertices.items[gl_VertexIndex]; // vertex pulling through a buffer address
    Vertex v = nova_skinned(gl_VertexIndex, bind);
    vec4 world = inst.model * vec4(v.position, 1.0);
    vec4 prevWorld = inst.prevModel * vec4(nova_skinnedPrevious(gl_VertexIndex, bind.position, v.position), 1.0);
#endif
    vWorldPos = world.xyz;
    vNormal = mat3(inst.normalMatrix) * v.normal;
    vUV = vec2(v.u, v.v);
    gl_Position = pc.frame.data.viewProj * world;
    vClip = gl_Position;
    vPrevClip = pc.frame.data.prevViewProj * prevWorld;
}
