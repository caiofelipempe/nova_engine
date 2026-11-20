// Shared GPU data layouts. Mirrored on the C++ side in nova/renderer/GpuTypes.h — keep in sync.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#define MAX_POINT_LIGHTS 16

#define FRAME_FLAG_RT_SHADOWS 1u

#define MATERIAL_FLAG_CHECKER 1u

struct PointLight {
    vec4 positionRadius;  // xyz position, w radius
    vec4 colorIntensity;  // rgb color, w intensity
};

struct FrameData {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;     // xyz, w = time (s)
    vec4 sunDirection;  // xyz = direction the light travels, w = intensity
    vec4 sunColor;      // rgb
    vec4 skyZenith;     // rgb, w = ambient intensity
    vec4 skyHorizon;
    vec4 groundColor;
    uint pointLightCount;
    uint flags;
    float shadowBias;
    float _pad0;
    PointLight pointLights[MAX_POINT_LIGHTS];
};

struct InstanceData {
    mat4 model;
    mat4 normalMatrix;
    vec4 baseColor;
    vec4 emissive;  // rgb, w = strength
    vec4 params;    // x metallic, y roughness, z flags (as float bits), w baseColor texture (bindless, < 0 = none)
};

struct Vertex {
    vec3 position;
    float u;
    vec3 normal;
    float v;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameBuffer { FrameData data; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer InstanceBuffer { InstanceData items[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer VertexBuffer { Vertex items[]; };

// Bindless resources (set 0). See rhi/vulkan/VulkanCommon.h.
layout(set = 0, binding = 0) uniform sampler2D g_textures[];
