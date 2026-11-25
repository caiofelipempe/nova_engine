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
constexpr u32 kFrameFlagShadowMaps = 2u, kFrameFlagPointShadowMaps = 4u; ///< FrameData::flags: the sun's shadow maps (`FRAME_FLAG_SHADOW_MAPS`).
constexpr u32 kShadowCascades = 4;       ///< Shadow map cascades (a 2 × 2 atlas).
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
    glm::vec4 wrapSize;      ///< xyz = size of the circular world per axis (m), w = WrapShift (NOVA_WRAP_RENDER_MODES builds).
    glm::mat4 shadowMatrices[kShadowCascades]; ///< Camera-relative world → each cascade's clip space.
    glm::vec4 shadowSplits;  ///< Where each cascade ends (distance along the view, m).
    glm::vec4 shadowTexels;  ///< Each cascade's texel size in the world (m): the normal offset.
    glm::vec4 shadowParams;  ///< x = atlas texture (bindless), y = depth bias, z = cascade resolution (texels), w = blend between cascades (part of each).
    glm::vec4 cameraForward; ///< xyz = the camera's view direction.
    u32 pointLightCount;     ///< Number of valid entries in pointLights.
    u32 flags;               ///< kFrameFlag* bits.
    f32 shadowBias;          ///< Shadow ray origin offset along the normal.
    f32 _pad0;               ///< Padding (std430).
    PointLight pointLights[kMaxPointLights]; ///< Point lights.
    glm::vec4 pointShadowParams;   ///< Point shadow maps: x atlas (bindless), y face resolution, z near plane, w depth bias.
    /// @brief Per point light (4 per vector): its row in the point shadow atlas, -1 none (ray traced
    ///        shadows, or lit), -2 it casts no shadows at all.
    glm::ivec4 pointShadowSlots[kMaxPointLights / 4];
    /// @brief Last frame's view-projection without jitter, from this frame's origin (motion vectors).
    glm::mat4 prevViewProj;
    glm::vec4 jitter; ///< xy = this frame's projection offset in NDC (TAA; 0 without it): viewProj includes it.
};
static_assert(sizeof(FrameData) ==
              256 + 8 * 16 + 4 * 64 + 4 * 16 + 16 + kMaxPointLights * 32 + 16 + kMaxPointLights * 4 + 64 + 16);

/// @brief Per-draw instance data, indexed by DrawPush::instanceIndex.
struct InstanceData {
    glm::mat4 model;        ///< Object → world.
    glm::mat4 normalMatrix; ///< Inverse-transpose of `model` (upper 3x3 used).
    glm::vec4 baseColor;    ///< Material base color.
    glm::vec4 emissive;     ///< rgb = emissive color, w = strength.
    glm::vec4 params;       ///< x = metallic, y = roughness, z = material flags (uint bits), w = base color texture index (-1 = none).
    glm::vec4 custom;       ///< Material::customParams.
    glm::vec4 extra;        ///< x = Material::customTexture index (-1 = none); yzw = center of the lap this copy is drawn in (camera-relative m).
    glm::vec4 maps;         ///< Bindless indices (-1 = none): x normal, y metallic-roughness, z occlusion, w emissive.
    glm::vec4 surface;      ///< x normalScale, y occlusionStrength, z alphaCutoff, w AlphaMode.
    glm::vec4 uvMatrix;     ///< The UV transform's 2×2 (rotation · scale), by columns: (xy) (zw).
    glm::vec4 uvOffset;     ///< xy = its offset.
    glm::uvec4 mesh;        ///< Device addresses (lo, hi): xy the vertices, zw the indices (alpha-tested shadows).
    glm::mat4 prevModel;    ///< `model` last frame, from this frame's origin (motion vectors; = model when unknown).
};
static_assert(sizeof(InstanceData) == 352);

/// @brief Push constants of the forward pass (`forward_push.glsl`).
struct DrawPush {
    u64 frame;         ///< Address of FrameData.
    u64 instances;     ///< Address of the InstanceData array.
    u64 vertices;      ///< Address of the mesh's Vertex array.
    u64 tlas;          ///< TLAS address for ray queries (0 = no ray tracing).
    u32 instanceIndex; ///< Index into `instances`.
    u32 debugView;     ///< DebugView value.
    u64 indices;       ///< 0: an indexed draw. Otherwise the mesh's index buffer, pulled by the vertex shader (per-triangle wrap).
    u64 skin;          ///< 0: rigid. Otherwise the mesh's SkinVertex array (skinned draw).
    u64 joints;        ///< The draw's joint matrices (mat4 each, mesh space), when `skin` is set.
    u64 visible;       ///< 0: one instance (instanceIndex). Otherwise a GPU-culled group: instance = visible[gl_InstanceIndex].
    u64 prevJoints;    ///< Skinned draws: last frame's joint matrices (motion vectors; 0: the same as `joints`).
};
static_assert(sizeof(DrawPush) == 80);

/// @brief One instance to cull on the GPU (cull.comp).
struct CullItem {
    glm::vec4 sphere; ///< Camera-relative center, radius.
    u32 group;        ///< Its group's draw command.
    u32 instance;     ///< Index into the frame's InstanceData.
    u32 pad0, pad1;
};
static_assert(sizeof(CullItem) == 32);

/// @brief A group's indirect draw (VkDrawIndexedIndirectCommand).
struct DrawIndexedCommand {
    u32 indexCount, instanceCount, firstIndex;
    i32 vertexOffset;
    u32 firstInstance;
};
static_assert(sizeof(DrawIndexedCommand) == 20);

/// @brief Parameters of a cull.comp dispatch.
struct CullParams {
    glm::vec4 planes[6];          ///< Frustum, normalized: inside where dot(n, p) + d >= 0.
    glm::mat4 occlusionViewProj;  ///< The Hi-Z pyramid's view (phase 1: last frame's).
    glm::vec4 occlusionOffset;    ///< xyz: added to the spheres for that view; w: 1 = test against the pyramid.
    glm::uvec4 levels[16];        ///< Pyramid levels: x width, y height, z offset (floats).
    glm::uvec4 info;              ///< x: level count.
};

/// @brief Push constants of cull.comp.
struct CullPush {
    u64 items, commands, visible, params, counters, hiz, states;
    u32 count;
    u32 phase; ///< 0 frustum only; 1, 2: the two occlusion phases.
};

/// @brief Push constants of hiz.comp (one level of the pyramid).
struct HizPush {
    u64 hiz;
    u32 depthTexture, fromDepth;
    u32 srcOffset, dstOffset;
    glm::uvec2 srcSize, dstSize;
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
    u32 bloomTexture = ~0u; ///< Bindless index of the bloom image (~0u: none).
    f32 bloomStrength = 0.0f; ///< How much of it is mixed in.
    u32 _pad0 = 0;            ///< Padding (the address below is 8-aligned).
    u64 autoExposure = 0;     ///< The view's ExposureState (0: fixed exposure).
};
static_assert(sizeof(TonemapPush) == 32 && offsetof(TonemapPush, autoExposure) == 24);

/// @brief A view's auto exposure (`ExposureBuffer` in common.glsl).
struct ExposureState {
    u32 histogram[256]; ///< log2 luminance; bin 0 = black.
    f32 adaptedLog2;    ///< The eased average log2 luminance (NaN: none yet).
    f32 exposure;       ///< The multiplier that brings it to the key value.
    f32 _pad[2];
};
static_assert(sizeof(ExposureState) == 1040);

/// @brief Push constants of exposure.comp.
struct ExposurePush {
    u64 state;            ///< Address of the view's ExposureState.
    u32 hdrTexture;       ///< Bindless index of the HDR image.
    u32 mode;             ///< 0 histogram, 1 average.
    glm::uvec2 size;      ///< The image's size.
    f32 minLog2, rangeLog2; ///< log2 luminance of bin 1 and the range of bins 1..255.
    f32 adapt;            ///< How far toward the new average this frame goes (1: all the way).
    f32 key;              ///< Luminance the average is brought to.
    f32 minExposure, maxExposure;
    f32 lowPercentile, highPercentile;
};
static_assert(sizeof(ExposurePush) == 56);

/// @brief Push constants of ssao.frag.
struct SsaoPush {
    u64 frame;            ///< Address of FrameData.
    u32 depthTexture;     ///< Bindless index of the depth buffer.
    u32 mode;             ///< 0 occlusion, 1 apply, 2 show the occlusion.
    u32 colorTexture;     ///< Modes 1, 2: the HDR image.
    u32 ambientTexture;   ///< Modes 1, 2: the ambient light's share of it.
    u32 aoTexture;        ///< Modes 1, 2: the occlusion.
    u32 noiseFrame;       ///< Turns the sampling pattern each frame (TAA averages it out); 0 = fixed.
    glm::vec2 texel;      ///< 1 / the image's size.
    f32 radius;           ///< Sampling radius (m).
    f32 intensity;        ///< The occlusion raised to this power.
};
static_assert(sizeof(SsaoPush) == 48);

/// @brief Push constants of taa.frag.
struct TaaPush {
    u32 color;       ///< Bindless index of this frame's HDR image.
    u32 velocity;    ///< The motion vectors (UV this frame − UV last frame).
    u32 depth;       ///< The depth buffer.
    u32 history;     ///< Last frame's result.
    glm::vec2 texel; ///< 1 / size.
    f32 feedback;    ///< How much of this frame goes in (0..1).
    u32 mode;        ///< 0 resolve, 1 no history (this frame as is), 2 show the motion (DebugView::Motion).
};
static_assert(sizeof(TaaPush) == 32);

/// @brief Push constants of fxaa.frag.
struct FxaaPush {
    u32 source;           ///< Bindless index of the tonemapped image.
    f32 subpix;           ///< Sub-pixel blending (0 off .. 1 soft).
    f32 edgeThreshold;    ///< Contrast needed, relative to the brightest neighbor.
    f32 edgeThresholdMin; ///< Contrast needed in dark areas.
    glm::vec2 texel;      ///< 1 / size.
};
static_assert(sizeof(FxaaPush) == 24);

/// @brief Push constants of bloom.frag.
struct BloomPush {
    u32 source, lower, mode; ///< mode: 0 first downsample, 1 downsample, 2 upsample.
    f32 threshold;
    glm::vec2 texel;
    f32 radius, knee;
};

} // namespace nova::gpu
