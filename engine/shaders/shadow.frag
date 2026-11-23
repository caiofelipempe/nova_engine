#version 460
// Shadow maps: casters with a cut-out material (alpha Mask) cast only where they are opaque.
// Their alpha as forward shading sees it (base color × texture, through the UV transform).
#include "nova/common.glsl"
#include "nova/forward_push.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in uint vInstance;

void main() {
    InstanceData inst = pc.instances.items[vInstance];
    const int tex = int(inst.params.w);
    const float alpha = inst.baseColor.a * (tex >= 0 ? texture(g_textures[nonuniformEXT(tex)], nova_materialUv(inst, vUV)).a : 1.0);
    if (alpha < inst.surface.z) discard;
}
