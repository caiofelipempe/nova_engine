#pragma once

/// @file
/// @brief Node animation: clips of position / rotation / scale curves, and the component that plays
///        them on an entity's descendants (glTF animations, or your own).

#include "nova/core/Base.h"
#include "nova/math/Fixed.h"
#include "nova/scene/EntityRef.h"

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

/// @brief A moment of a clip that tells scripts something happened (a footstep, a hit frame).
struct AnimationEvent {
    f32 time = 0.0f;  ///< Seconds into the clip.
    std::string name; ///< What it is ("footstep").
    f32 value = 0.0f; ///< Anything more (which foot...).
};

/// @brief An animation event the playback passed (Script::onAnimationEvent, Scene::animationEvents).
struct AnimationEventHit {
    std::string name;  ///< AnimationEvent::name.
    f32 value = 0.0f;  ///< AnimationEvent::value.
    std::string clip;  ///< The clip's name.
    f32 time = 0.0f;   ///< The event's time in the clip.
};

/// @brief A named animation: channels over a duration, and events.
///
/// glTF has no events: add them to a clip you play (for a model's clip, a copy:
/// `auto c = std::make_shared<AnimationClip>(*clip); c->events.push_back({0.4f, "footstep"});`).
struct AnimationClip {
    std::string name;
    f32 duration = 0.0f; ///< Seconds (the last key of any channel).
    std::vector<AnimationChannel> channels;
    std::vector<AnimationEvent> events; ///< Fired when the playback passes their time (any order).
};

/// @brief How a mixed-in clip combines with the others (AnimationMix::mode).
enum class AnimationBlend : u8 {
    Average,  ///< Averaged with the main clip and the other averaged layers by weight.
    Override, ///< Replaces what the others made by its weight (0..1), after them, in order: with a
              ///< mask, a part of the skeleton plays its own clip (waving while walking).
};

/// @brief How Scene::blendAnimation mixes a clip in.
struct AnimationMix {
    f32 weight = 1.0f;                  ///< Average: relative to the main clip's 1. Override: 0..1.
    bool sync = true;                   ///< Follows the main clip's phase.
    AnimationBlend mode = AnimationBlend::Average;
    /// @brief The nodes it moves, as paths from the animated entity ("Hips/Spine"): each covers
    ///        that node and everything under it. Empty: the whole skeleton.
    std::vector<std::string> mask;
    bool events = false;                ///< Fires its clip's events too (only the main clip's otherwise).
};

/// @brief True when channel target `path` is in `mask` (one of its paths or under one; an empty
///        mask holds everything).
bool inMask(const std::vector<std::string>& mask, const std::string& path);

/// @brief A clip playing beside an animator's own: fading out after a change of clip, or mixed in
///        with a weight (Scene::playAnimation, Scene::blendAnimation). Not saved.
struct AnimationLayer {
    std::shared_ptr<const AnimationClip> data; ///< The clip.
    f64 time = 0.0;                            ///< Position in it (seconds).
    f32 speed = 1.0f;                          ///< Playback rate.
    bool loop = true;                          ///< Wrap around at the end.
    f32 weight = 1.0f;                         ///< Its share of the mix (relative to the others).
    f32 fade = 0.0f;                           ///< Weight change per second (negative: fading out, gone at 0).
    bool sync = false;                         ///< Follows the main clip's phase (walk and run in step).
    AnimationBlend mode = AnimationBlend::Average; ///< Averaged in, or overriding the others.
    std::vector<std::string> mask;             ///< The nodes it moves (AnimationMix::mask); empty: all.
    bool events = false;                       ///< Fires its clip's events (never while fading out).
    const AnimationClip* boundClip = nullptr;  ///< The clip `targets` were found for.
    std::vector<entt::entity> targets;         ///< One per channel.
};

/// @brief Plays a clip on this entity's descendants (and itself): each channel sets the local
///        pose (offset from the parent) of the entity its `target` path names.
///
/// Clips can also be changed with a cross-fade (Scene::playAnimation) and mixed with others by
/// weight (Scene::blendAnimation): per node and property, the clips' values are averaged by
/// weight (rotations as quaternions); what no clip animates keeps its pose.
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
    f32 weight = 1.0f;                          ///< The main clip's share of the mix.
    f32 fade = 0.0f;                            ///< Its weight change per second (fading in to 1).
    bool started = true;                        ///< Not played yet: events at its start fire on the first update.
    std::vector<AnimationLayer> layers;         ///< Fading out and mixed-in clips.
};

/// @brief A skeleton for skinned meshes: its joints and how each one sat in the bind pose.
struct SkinData {
    std::string name;
    /// @brief Each joint, as the path of entity names from the model's root ("Hips/Spine/Head").
    std::vector<std::string> joints;
    /// @brief Per joint: mesh bind-pose space → the joint's space (glTF inverseBindMatrices).
    std::vector<glm::mat4> inverseBind;
};

/// @brief Deforms this entity's skinned mesh (MeshData::skin) by a skeleton: the joints are
///        entities under `root` (the model's root), moved by an AnimatorComponent, scripts or tools.
///
/// Added by instantiateModel / AssetDatabase::instantiate to the meshes of skinned glTF nodes.
/// The skeleton is set directly (`data`) or named by `skin`, a reference the scene resolves through
/// its skin source (Scene::setSkinSource; model assets: "asset:<guid>#<index>"). Only `root` and
/// `skin` are saved. Ray-traced shadows and picking still see the bind pose.
struct SkinComponent {
    EntityRef root;    ///< The entity the joint paths start from (the model's root).
    std::string skin;  ///< Reference to the skeleton (resolved through the scene's skin source).

    // Kept by the scene (not saved):
    std::shared_ptr<const SkinData> data;   ///< The skeleton (set it to use one directly).
    std::string resolved;                   ///< The `skin` reference `data` came from.
    const SkinData* boundSkin = nullptr;    ///< The skeleton `joints` were found for.
    u64 boundRoot = 0;                      ///< ... from this root.
    std::vector<entt::entity> joints;       ///< One per joint (entt::null: not found).
};

} // namespace nova
