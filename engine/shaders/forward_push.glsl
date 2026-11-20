// Push constants for mesh draws (C++: nova::gpu::DrawPush).
layout(push_constant) uniform DrawPush {
    FrameBuffer frame;
    InstanceBuffer instances;
    VertexBuffer vertices;
    uint64_t tlas;        // top-level acceleration structure address (0 = none)
    uint instanceIndex;
    uint debugView;       // 0 lit, 1 normals, 2 shadows only, 3 albedo
} pc;
