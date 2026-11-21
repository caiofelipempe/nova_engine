// Push constants of mesh draws (C++: nova::gpu::DrawPush).
#ifndef NOVA_FORWARD_PUSH_GLSL
#define NOVA_FORWARD_PUSH_GLSL
#include "nova/common.glsl"

layout(push_constant) uniform DrawPush {
    FrameBuffer frame;
    InstanceBuffer instances;
    VertexBuffer vertices;
    uint64_t tlas;        // top-level acceleration structure address (0 = none)
    uint instanceIndex;
    uint debugView;       // 0 lit, 1 normals, 2 shadows only, 3 albedo
} pc;

#endif // NOVA_FORWARD_PUSH_GLSL
