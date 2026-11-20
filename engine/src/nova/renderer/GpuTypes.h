#pragma once

#include "nova/core/Base.h"

#include <glm/glm.hpp>

// CPU mirrors of the structures in shaders/common.glsl (std430). Keep both in sync.
namespace nova::gpu {

constexpr u32 kMaxPointLights = 16;
constexpr u32 kFrameFlagRtShadows = 1u;
constexpr u32 kMaterialFlagChecker = 1u;

struct PointLight {
    glm::vec4 positionRadius;
    glm::vec4 colorIntensity;
};

struct FrameData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::vec4 cameraPos;
    glm::vec4 sunDirection;
    glm::vec4 sunColor;
    glm::vec4 skyZenith;
    glm::vec4 skyHorizon;
    glm::vec4 groundColor;
    u32 pointLightCount;
    u32 flags;
    f32 shadowBias;
    f32 _pad0;
    PointLight pointLights[kMaxPointLights];
};
static_assert(sizeof(FrameData) == 256 + 6 * 16 + 16 + kMaxPointLights * 32);

struct InstanceData {
    glm::mat4 model;
    glm::mat4 normalMatrix;
    glm::vec4 baseColor;
    glm::vec4 emissive;
    glm::vec4 params; // metallic, roughness, flags (uint bits), texture index (float, -1 = none)
};
static_assert(sizeof(InstanceData) == 176);

struct DrawPush {
    u64 frame;
    u64 instances;
    u64 vertices;
    u64 tlas;
    u32 instanceIndex;
    u32 debugView;
};

struct SkyPush {
    u64 frame;
};

struct TonemapPush {
    u32 hdrTexture;
    f32 exposure;
    u32 tonemapper;
};

} // namespace nova::gpu
