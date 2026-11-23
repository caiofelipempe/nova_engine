#pragma once

/// @file
/// @brief Node animation: clips of position / rotation / scale curves, and the component that plays
///        them on an entity's descendants (glTF animations, or your own).

#include "nova/core/Base.h"
#include "nova/math/Fixed.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>
#include <vector>

namespace nova {

/// @brief What an animation channel moves.
enum class AnimationPath : u8 {
    Translation, ///< The node's position in its parent's frame (LocalTransform::position).
    Rotation,    ///< Its rotation relative to the parent (LocalTransform::rotation).
    Scale,       ///< Its scale relative to the parent (LocalTransform::scale).
};

/// @brief How a channel goes from one key to the next (as glTF).
enum class AnimationInterpolation : u8 {
    Step,        ///< Holds each key until the next.
    Linear,      ///< Straight lines (rotations: slerp).
    CubicSpline, ///< Hermite curves through the keys, with in / out tangents per key.
};

/// @brief One curve of a clip: one property of one node over time.
struct AnimationChannel {
    /// @brief The node, as the names of the entities from the animated entity down to it, joined
    ///        by '/' ("Body/Axle/Wheel"); empty = the animated entity itself.
    std::string target;
    AnimationPath path = AnimationPath::Translation;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    std::vector<f32> times; ///< Key times (seconds, increasing).
    /// @brief Key values: xyz for translation and scale, xyzw (a quaternion) for rotation. For
    ///        CubicSpline, three per key: in-tangent, value, out-tangent.
    std::vector<glm::vec4> values;

    /// @brief The value at `time` (clamped to the keys).
    glm::vec4 sample(f32 time) const;
};

/// @brief A named animation: channels over a duration.
struct AnimationClip {
    std::string name;
    f32 duration = 0.0f; ///< Seconds (the last key of any channel).
    std::vector<AnimationChannel> channels;
};

/// @brief Plays a clip on this entity's descendants (and itself): each channel sets the local
///        pose (offset from the parent) of the entity its `target` path names.
///
/// Added to a model's root by instantiateModel / AssetDatabase::instantiate when the model has
/// animations. The clip is either set directly (`data`) or named by `clip`, a reference the
/// scene resolves through its animation source (Scene::setAnimationSource; for model assets,
/// "asset:<guid>#<index>", see AssetDatabase::animationSource()). Only `clip`, `speed`, `loop`
/// and `playing` are saved: a loaded scene plays from the start.
struct AnimatorComponent {
    std::string clip;  ///< Reference to the clip (resolved through the scene's animation source).
    Scalar speed = 1;  ///< Playback rate (1 = real time; negative plays backwards).
    bool loop = true;  ///< Wrap around at the end (otherwise hold the last pose).
    bool playing = true;

    // Kept by the scene (not saved):
    f64 time = 0.0;                             ///< Position in the clip (seconds).
    std::shared_ptr<const AnimationClip> data;  ///< The clip being played (set it to play a clip directly).
    std::string resolved;                       ///< The `clip` reference `data` came from.
    const AnimationClip* boundClip = nullptr;   ///< The clip `targets` were found for.
    std::vector<entt::entity> targets;          ///< One per channel (entt::null: not found).
};

} // namespace nova
