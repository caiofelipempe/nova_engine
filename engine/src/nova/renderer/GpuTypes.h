#pragma once

/// @file
/// @brief CPU mirrors of the GPU data layouts in shaders/nova/common.glsl (std430).
/// @warning Keep these structs and the GLSL declarations in sync; sizes are checked with static_assert.

#include "nova/core/Base.h"

#include <glm/glm.hpp>

#include <cstddef>

/// @brief Shader-visible data layouts used by the Renderer.
namespace nova::gpu {

constexpr u32 kMaxPointLights = 16;      ///< Point lights per frame (`MAX_POINT_LIGHTS`).
constexpr u32 kFrameFlagRtShadows = 1u;  ///< FrameData::flags: trace shadow rays (`FRAME_FLAG_RT_SHADOWS`).
constexpr u32 kMaterialFlagChecker = 1u; ///< InstanceData::params.z bit: checker pattern (`MATERIAL_FLAG_CHECKER`).
/// @brief Period (m) of the world position shaders see (`NOVA_WORLD_TILE`): patterns whose period divides it are seamless.
constexpr f32 kWorldTile = 4096.0f;

/// @brief One point light as seen by shaders.
struct PointLight {
    glm::vec4 positionRadius; ///< xyz = world position, w = radius.
    glm::vec4 colorIntensity; ///< rgb = color, w = intensity.
};

/// @brief Per-frame constants, read through a device address.
struct FrameData {
    glm::mat4 view;          ///< (World − origin) → view: every position the GPU sees is camera-relative.
    glm::mat4 proj;          ///< View → clip.
    glm::mat4 viewProj;      ///< World → clip.
    glm::mat4 invViewProj;   ///< Clip → world (sky ray reconstruction).
    glm::vec4 cameraPos;     ///< xyz = camera position, w = time in seconds.
    glm::vec4 sunDirection;  ///< xyz = direction the light travels (normalized), w = intensity.
    glm::vec4 sunColor;      ///< rgb = sun color.
    glm::vec4 skyZenith;     ///< rgb = zenith color, w = ambient intensity.
    glm::vec4 skyHorizon;    ///< rgb = horizon color.
    glm::vec4 groundColor;   ///< rgb = ground color.
    glm::vec4 originTiled;   ///< xyz = frame origin (camera) modulo kWorldTile, w = kWorldTile.
    u32 pointLightCount;     ///< Number of valid entries in pointLights.
    u32 flags;               ///< kFrameFlag* bits.
    f32 shadowBias;          ///< Shadow ray origin offset along the normal.
    f32 _pad0;               ///< Padding (std430).
    PointLight pointLights[kMaxPointLights]; ///< Point lights.
};
static_assert(sizeof(FrameData) == 256 + 7 * 16 + 16 + kMaxPointLights * 32);

/// @brief Per-draw instance data, indexed by DrawPush::instanceIndex.
struct InstanceData {
    glm::mat4 model;        ///< Object → world.
    glm::mat4 normalMatrix; ///< Inverse-transpose of `model` (upper 3x3 used).
    glm::vec4 baseColor;    ///< Material base color.
    glm::vec4 emissive;     ///< rgb = emissive color, w = strength.
    glm::vec4 params;       ///< x = metallic, y = roughness, z = material flags (uint bits), w = base color texture index (-1 = none).
    glm::vec4 custom;       ///< Material::customParams.
    glm::vec4 extra;        ///< x = Material::customTexture index (-1 = none); yzw reserved.
};
static_assert(sizeof(InstanceData) == 208);

/// @brief Push constants of the forward pass (`forward_push.glsl`).
struct DrawPush {
    u64 frame;         ///< Address of FrameData.
    u64 instances;     ///< Address of the InstanceData array.
    u64 vertices;      ///< Address of the mesh's Vertex array.
    u64 tlas;          ///< TLAS address for ray queries (0 = no ray tracing).
    u32 instanceIndex; ///< Index into `instances`.
    u32 debugView;     ///< DebugView value.
};

/// @brief Push constants of the sky pass.
struct SkyPush {
    u64 frame; ///< Address of FrameData.
};

/// @brief Push constants of a post-process pass (`nova/postprocess.glsl`).
struct PostProcessPush {
    u64 frame;            ///< Address of FrameData.
    u32 inputTexture;     ///< Bindless index of the HDR input.
    u32 _pad0;            ///< Padding.
    glm::vec2 texelSize;  ///< 1 / resolution.
    glm::vec2 _pad1;      ///< Padding: GLSL aligns the vec4 below to 16 bytes.
    glm::vec4 params;     ///< PostProcessDesc::params.
};
static_assert(sizeof(PostProcessPush) == 48 && offsetof(PostProcessPush, params) == 32);

/// @brief Push constants of the tonemap pass.
struct TonemapPush {
    u32 hdrTexture; ///< Bindless index of the HDR color target.
    f32 exposure;   ///< Exposure multiplier.
    u32 tonemapper; ///< Tonemapper value.
};

} // namespace nova::gpu
