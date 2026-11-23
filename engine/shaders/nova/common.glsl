// Shared GPU data layouts. Mirrored on the C++ side in nova/renderer/GpuTypes.h — keep in sync.
// Public: game shaders may include it (#include "nova/common.glsl").
#ifndef NOVA_COMMON_GLSL
#define NOVA_COMMON_GLSL
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#define MAX_POINT_LIGHTS 16

#define FRAME_FLAG_RT_SHADOWS 1u
#define FRAME_FLAG_SHADOW_MAPS 2u
#define FRAME_FLAG_POINT_SHADOW_MAPS 4u

#define MATERIAL_FLAG_CHECKER 1u

// Positions on the GPU are relative to the frame origin (the camera). Shaders that need world
// coordinates (patterns, noise) use the world position modulo this tile: seamless for any period
// that divides it, and precise however far the camera is from the world origin.
#define NOVA_WORLD_TILE 4096.0

struct PointLight {
    vec4 positionRadius;  // xyz position, w radius
    vec4 colorIntensity;  // rgb color, w intensity
};

struct FrameData {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;     // xyz relative to the frame origin (zero for camera-relative frames), w = time (s)
    vec4 sunDirection;  // xyz = direction the light travels, w = intensity
    vec4 sunColor;      // rgb
    vec4 skyZenith;     // rgb, w = ambient intensity
    vec4 skyHorizon;
    vec4 groundColor;
    vec4 originTiled;   // xyz = frame origin modulo NOVA_WORLD_TILE, w = NOVA_WORLD_TILE
    vec4 wrapSize;      // xyz = size of the circular world per axis (m), w = wrap mode (NOVA_WRAP_RENDER_MODES)
    mat4 shadowMatrices[4]; // camera-relative world -> each cascade's clip space
    vec4 shadowSplits;  // where each cascade ends (distance along the view, m)
    vec4 shadowTexels;  // each cascade's texel size in the world (m)
    vec4 shadowParams;  // x = atlas texture (bindless), y = depth bias, z = cascade resolution
    vec4 cameraForward; // xyz = the camera's view direction
    uint pointLightCount;
    uint flags;
    float shadowBias;
    float _pad0;
    PointLight pointLights[MAX_POINT_LIGHTS];
    vec4 pointShadowParams;  // point shadow maps: x atlas (bindless), y face resolution, z near plane, w depth bias
    ivec4 pointShadowSlots[MAX_POINT_LIGHTS / 4]; // per light: its atlas row, -1 none, -2 casts no shadows
};

struct InstanceData {
    mat4 model;
    mat4 normalMatrix;
    vec4 baseColor;
    vec4 emissive;  // rgb, w = strength
    vec4 params;    // x metallic, y roughness, z flags (as float bits), w baseColor texture (bindless, < 0 = none)
    vec4 custom;    // Material::customParams (free for game material shaders)
    vec4 extra;     // x = Material::customTexture (bindless, < 0 = none), yzw = center of the lap of this copy
    vec4 maps;      // bindless (< 0 = none): x normal, y metallic-roughness, z occlusion, w emissive
    vec4 surface;   // x normalScale, y occlusionStrength, z alphaCutoff, w alpha mode (0 opaque, 1 mask, 2 blend)
    vec4 uvMatrix;  // the UV transform's 2x2 by columns: (xy) (zw)
    vec4 uvOffset;  // xy = its offset
    uvec4 mesh;     // device addresses (lo, hi): xy vertices, zw indices
};

// A mesh UV through the material's UV transform (Material::uvScale, uvRotation, uvOffset).
vec2 nova_materialUv(InstanceData inst, vec2 uv) { return mat2(inst.uvMatrix.xy, inst.uvMatrix.zw) * uv + inst.uvOffset.xy; }

struct Vertex {
    vec3 position;
    float u;
    vec3 normal;
    float v;
};

// A skinned vertex: joints.x = joint 0 | joint 1 << 16, joints.y = joint 2 | joint 3 << 16.
struct SkinVertex {
    uvec4 joints;
    vec4 weights;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameBuffer { FrameData data; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer InstanceBuffer { InstanceData items[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinBuffer { SkinVertex items[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointBuffer { mat4 items[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer VertexBuffer { Vertex items[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IndexBuffer { uint items[]; };

// Bindless resources (set 0). See rhi/vulkan/VulkanCommon.h.
layout(set = 0, binding = 0) uniform sampler2D g_textures[];

#endif // NOVA_COMMON_GLSL
