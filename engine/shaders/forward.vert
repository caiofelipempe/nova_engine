#version 460
#include "nova/common.glsl"
#include "nova/forward_push.glsl"

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

#if NOVA_WRAP_RENDER_MODES
// Objects across a seam of the circular world (RenderSettings::wrapShift). Positions are camera
// relative; `cell` is the center of the lap this copy is drawn in (0 for the lap around the camera).
#include "nova/wrap.glsl"

out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[3];
};

#endif

void main() {
    InstanceData inst = pc.instances.items[pc.instanceIndex];
#if NOVA_WRAP_RENDER_MODES
    const vec3 size = pc.frame.data.wrapSize.xyz;
    const uint mode = uint(pc.frame.data.wrapSize.w);
    const vec3 cell = inst.extra.yzw;
#if NOVA_RENDER_ORIGIN_ROTATION
    // The lap around the camera turning with it (RenderSettings::wrapTurnWithCamera).
    const bool turned = (pc.frame.data.flags & FRAME_FLAG_TURN_LAP) != 0u;
#else
    const bool turned = false;
#endif
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
        const vec3 shift = turned ? nova_turnedLapShift(middle - cell, size, mat3(pc.frame.data.view))
                                  : nova_lapShift(middle - cell, size);
        world = vec4(p[corner] + shift, 1.0);
    } else {
        v = pc.vertices.items[gl_VertexIndex];
        world = inst.model * vec4(v.position, 1.0);
    }
    // Split and crop: every copy is cut at the edges of its lap (the planes half a lap from its center).
    // (Turned, they are cut per fragment instead: the turned lap is not a box of the world's axes.)
    const bool clipped = !turned && (mode == NOVA_WRAP_SPLIT || mode == NOVA_WRAP_CROP);
    for (int i = 0; i < 3; ++i)
        gl_ClipDistance[i] = (clipped && size[i] > 0.0) ? size[i] * 0.5 - abs(world[i] - cell[i]) : 1.0;
#if NOVA_RENDER_ORIGIN_ROTATION
    // Crop turning with the camera: a box in its own axes (positions are camera-relative, so the
    // view matrix only turns them), small enough to fit in the lap around the camera.
    if (mode == NOVA_WRAP_CROP_VIEW) {
        const vec3 seen = mat3(pc.frame.data.view) * world.xyz;
        for (int i = 0; i < 3; ++i) gl_ClipDistance[i] = pc.frame.data.cropHalf - abs(seen[i]);
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
