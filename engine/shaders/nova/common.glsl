// Shared GPU data layouts. Mirrored on the C++ side in nova/renderer/GpuTypes.h — keep in sync.
// Public: game shaders may include it (#include "nova/common.glsl").
#ifndef NOVA_COMMON_GLSL
#define NOVA_COMMON_GLSL
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#define MAX_POINT_LIGHTS 16

#define FRAME_FLAG_RT_SHADOWS 1u
#define FRAME_FLAG_TURN_LAP 2u // the lap around the camera turns with it (per triangle, split, crop)
#define FRAME_FLAG_TURN_CUT 4u // fragments outside their turned lap are discarded (split, crop)

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
    uint pointLightCount;
    uint flags;
    float shadowBias;
    float cropHalf;     // WrapCrop::View: half a side of the crop box in the camera's axes (m)
    PointLight pointLights[MAX_POINT_LIGHTS];
};

struct InstanceData {
    mat4 model;
    mat4 normalMatrix;
    vec4 baseColor;
    vec4 emissive;  // rgb, w = strength
    vec4 params;    // x metallic, y roughness, z flags (as float bits), w baseColor texture (bindless, < 0 = none)
    vec4 custom;    // Material::customParams (free for game material shaders)
    vec4 extra;     // x = Material::customTexture (bindless, < 0 = none), yzw = center of the lap of this copy
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
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IndexBuffer { uint items[]; };

// Bindless resources (set 0). See rhi/vulkan/VulkanCommon.h.
layout(set = 0, binding = 0) uniform sampler2D g_textures[];

#endif // NOVA_COMMON_GLSL
