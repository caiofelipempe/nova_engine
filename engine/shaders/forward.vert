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
#define NOVA_WRAP_CROP_VIEW 4u // crop in the camera's own axes (NOVA_RENDER_ORIGIN_ROTATION builds)
#define NOVA_WRAP_SPLIT_SLAB 5u // + the slab's axis (5, 6, 7): split turning with the camera

#if NOVA_RENDER_ORIGIN_ROTATION
#define NOVA_CLIP_PLANES 6 // the lap's box (0..2) and the crop box turning with the camera (3..5)
#else
#define NOVA_CLIP_PLANES 3
#endif
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[NOVA_CLIP_PLANES];
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
    // Split turning with the camera: along the world axis nearest the camera's direction the lap is
    // a slab one lap thick, its faces square to that direction (|direction · q| within half its
    // thickness); across it, the lap as usual. It still tiles the world: one lap along that axis
    // for every point across.
    if (mode >= NOVA_WRAP_SPLIT_SLAB && mode <= NOVA_WRAP_SPLIT_SLAB + 2u) {
        const int axis = int(mode - NOVA_WRAP_SPLIT_SLAB);
        const vec3 q = world.xyz - cell;
        for (int i = 0; i < 3; ++i)
            gl_ClipDistance[i] = (i != axis && size[i] > 0.0) ? size[i] * 0.5 - abs(q[i]) : 1.0;
        gl_ClipDistance[axis] = pc.frame.data.wrapSlab.w - abs(dot(pc.frame.data.wrapSlab.xyz, q));
    }
#if NOVA_RENDER_ORIGIN_ROTATION
    // Crop turning with the camera: a box in its own axes (positions are camera-relative, so the
    // view matrix only turns them), cut by the lap around the camera too. As big as fits in that
    // lap, it is all there is; bigger (RenderSettings::wrapCropViewSize), its corners are trimmed
    // by the lap's faces: each object is drawn once, at its copy nearest the camera.
    for (int i = 0; i < 3; ++i) gl_ClipDistance[3 + i] = 1.0;
    if (mode == NOVA_WRAP_CROP_VIEW) {
        const vec3 seen = mat3(pc.frame.data.view) * world.xyz;
        for (int i = 0; i < 3; ++i) {
            gl_ClipDistance[i] = size[i] > 0.0 ? size[i] * 0.5 - abs(world[i]) : 1.0;
            gl_ClipDistance[3 + i] = pc.frame.data.cropHalf - abs(seen[i]);
        }
    }
#endif
#else
    Vertex v = pc.vertices.items[gl_VertexIndex]; // vertex pulling through a buffer address
    vec4 world = inst.model * vec4(v.position, 1.0);
#endif
    vWorldPos = world.xyz;
    vNormal = mat3(inst.normalMatrix) * v.normal;
    vUV = vec2(v.u, v.v);
    gl_Position = pc.frame.data.viewProj * world;
}
