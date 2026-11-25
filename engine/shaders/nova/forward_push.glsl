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
    IndexBuffer indices;  // 0 = indexed draw; else the mesh's indices (the vertex shader wraps whole triangles)
    SkinBuffer skin;      // 0 = rigid; else the mesh's skin weights
    JointBuffer joints;   // the draw's joint matrices (mesh space)
    IndexBuffer visible;  // 0 = one instance (instanceIndex); else GPU culled: the instance is visible[gl_InstanceIndex]
    JointBuffer prevJoints; // skinned: last frame's joint matrices (motion vectors; 0 = the same as joints)
} pc;

#endif // NOVA_FORWARD_PUSH_GLSL
