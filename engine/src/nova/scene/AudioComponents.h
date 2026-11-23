#pragma once

/// @file
/// @brief Sound in the scene: sources on entities and the listener.

#include "nova/audio/Audio.h"

#include <memory>
#include <string>

namespace nova {

/// @brief Plays a sound from this entity (where it is, if `spatial`).
///
/// The clip is set directly (`data`) or named by `clip`, a reference the scene resolves through
/// its sound source (Scene::setSoundSource; sound assets: "asset:<guid>"). Scripts start and stop
/// it with Scene::playSource / stopSource. Only the settings are saved.
struct AudioSourceComponent {
    std::string clip;          ///< Reference to the clip.
    f32 volume = 1.0f;         ///< Linear gain.
    f32 pitch = 1.0f;          ///< Playback rate.
    bool loop = false;
    bool playOnStart = true;   ///< Starts by itself on the first update.
    bool spatial = true;       ///< Placed at the entity (attenuated and panned) or heard flat (music).
    f32 minDistance = 1.0f;    ///< Full volume up to here (m).
    f32 maxDistance = 50.0f;   ///< Silent from here on (m).

    // Kept by the scene (not saved):
    std::shared_ptr<const AudioClip> data; ///< The clip (set it to play one directly).
    std::string resolved;                  ///< The `clip` reference `data` came from.
    VoiceId voice;                         ///< Playing voice (0: none).
    bool started = false;                  ///< playOnStart done.
};

/// @brief Marks the entity sound is heard from (else the primary camera).
struct AudioListenerComponent {
    bool active = true;
};

} // namespace nova
