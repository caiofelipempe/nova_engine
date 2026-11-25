// Material vertex shader interface (Material::vertexShader): moves the mesh's vertices before they
// are placed in the world (wind, waves, wobble). Lighting, shadows and the rest stay the engine's:
//
//   #version 460
//   #include "nova/vertex.glsl"
//
//   void vertex(inout NovaVertex v) {
//       v.position.y += sin(v.position.x * 4.0 + nova_time()) * 0.05;   // object space
//   }
//
// `v` arrives in object space (after skinning). The engine's own forward.vert is exactly this file
// with an empty vertex(). The renderer uses the same shader for the object's shadow maps.
// Limits: objects with a vertex shader skip GPU culling and the depth prepass (their bounds would
// not hold); ray-traced shadows and reflections see the undeformed mesh; motion vectors move last
// frame's position by this frame's displacement.
#ifndef NOVA_VERTEX_GLSL
#define NOVA_VERTEX_GLSL
#include "nova/common.glsl"
#include "nova/forward_push.glsl"

struct NovaVertex {
    vec3 position; // object space
    vec3 normal;   // object space (need not stay unit: the engine normalizes it)
    vec2 uv;       // the mesh UV (before the material's UV transform)
    uint instance; // read-only: the instance being drawn (nova_instance())
};

// ---- helpers for vertex code
InstanceData nova_vertexInstance(uint instance) { return pc.instances.items[instance]; }
float nova_time() { return pc.frame.data.cameraPos.w; }
// Material::customParams of the instance.
vec4 nova_custom(uint instance) { return pc.instances.items[instance].custom; }
// The world position of an object-space point modulo NOVA_WORLD_TILE (4096 m): for patterns and
// noise that should stay put in the world (waves on several objects lining up).
vec3 nova_worldPosition(uint instance, vec3 objectPosition) {
    return (pc.instances.items[instance].model * vec4(objectPosition, 1.0)).xyz + pc.frame.data.originTiled.xyz;
}

// Implemented by the material vertex shader.
void vertex(inout NovaVertex v);

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

// The material's vertex(): returns the moved vertex and how far it moved (for last frame's position).
Vertex nova_applyVertex(Vertex v, uint instance, out vec3 moved) {
    NovaVertex nv;
    nv.position = v.position;
    nv.normal = v.normal;
    nv.uv = vec2(v.u, v.v);
    nv.instance = instance;
    vertex(nv);
    moved = nv.position - v.position;
    v.position = nv.position;
    v.normal = nv.normal;
    v.u = nv.uv.x;
    v.v = nv.uv.y;
    return v;
}

void main() {
    // A draw of one instance, or of a group culled on the GPU (its visible instances, in a list).
    const uint instanceIndex = uint64_t(pc.visible) != 0ul ? pc.visible.items[gl_InstanceIndex] : pc.instanceIndex;
    vInstance = instanceIndex;
    InstanceData inst = pc.instances.items[instanceIndex];
    vec3 moved;
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
            vec3 movedK;
            Vertex vk = nova_applyVertex(nova_skinned(index, bind), instanceIndex, movedK);
            p[k] = (inst.model * vec4(vk.position, 1.0)).xyz;
            if (k == corner) {
                v = vk;
                prevCorner = (inst.prevModel * vec4(nova_skinnedPrevious(index, bind.position, vk.position - movedK) + movedK, 1.0)).xyz;
            }
        }
        const vec3 middle = (p[0] + p[1] + p[2]) / 3.0; // the centroid
        const vec3 shift = lapShift(middle - cell, size);
        world = vec4(p[corner] + shift, 1.0);
        prevWorld = vec4(prevCorner + shift, 1.0); // the same lap as now
    } else {
        const Vertex bind = pc.vertices.items[gl_VertexIndex];
        v = nova_applyVertex(nova_skinned(gl_VertexIndex, bind), instanceIndex, moved);
        world = inst.model * vec4(v.position, 1.0);
        prevWorld = inst.prevModel * vec4(nova_skinnedPrevious(gl_VertexIndex, bind.position, v.position - moved) + moved, 1.0);
    }
    // Split and crop: every copy is cut at the edges of its lap (the planes half a lap from its center).
    const bool clipped = mode == NOVA_WRAP_SPLIT || mode == NOVA_WRAP_CROP;
    for (int i = 0; i < 3; ++i)
        gl_ClipDistance[i] = (clipped && size[i] > 0.0) ? size[i] * 0.5 - abs(world[i] - cell[i]) : 1.0;
#else
    const Vertex bind = pc.vertices.items[gl_VertexIndex]; // vertex pulling through a buffer address
    Vertex v = nova_applyVertex(nova_skinned(gl_VertexIndex, bind), instanceIndex, moved);
    vec4 world = inst.model * vec4(v.position, 1.0);
    vec4 prevWorld = inst.prevModel * vec4(nova_skinnedPrevious(gl_VertexIndex, bind.position, v.position - moved) + moved, 1.0);
#endif
    vWorldPos = world.xyz;
    vNormal = mat3(inst.normalMatrix) * v.normal;
    vUV = vec2(v.u, v.v);
    gl_Position = pc.frame.data.viewProj * world;
    vClip = gl_Position;
    vPrevClip = pc.frame.data.prevViewProj * prevWorld;
}

#endif // NOVA_VERTEX_GLSL
