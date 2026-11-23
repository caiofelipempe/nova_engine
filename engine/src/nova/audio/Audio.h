#pragma once

/// @file
/// @brief Sound: decoded clips, and the output that plays them (miniaudio backend, or silent).

#include "nova/core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nova {

/// @brief A sound, decoded: interleaved 32-bit float samples (backend independent).
struct AudioClip {
    std::string name;
    std::vector<f32> samples; ///< Interleaved frames (`channels` samples each).
    u32 channels = 1;
    u32 sampleRate = 48000;

    /// @brief Frames (samples per channel).
    u64 frames() const { return channels ? samples.size() / channels : 0; }
    /// @brief Length in seconds.
    f64 duration() const { return sampleRate ? static_cast<f64>(frames()) / sampleRate : 0.0; }
};

/// @brief Decodes a WAV, MP3 or FLAC file. Null (logged) on failure.
std::shared_ptr<const AudioClip> decodeAudioFile(const std::filesystem::path& path);
/// @brief Same, from the file's bytes in memory.
std::shared_ptr<const AudioClip> decodeAudioMemory(const void* data, size_t size, const std::string& name = {});

/// @brief A playing sound (Audio::play); 0 = none.
struct VoiceId {
    u32 value = 0;
    explicit operator bool() const { return value != 0; }
    bool operator==(const VoiceId&) const = default;
};

/// @brief How a sound plays (Audio::play, Audio::update).
struct VoiceParams {
    f32 volume = 1.0f; ///< Linear gain.
    f32 pitch = 1.0f;  ///< Playback rate (2 = an octave up).
    bool loop = false;
    /// @brief Placed in the world (attenuated by distance, panned by direction) or not (music, UI).
    bool spatial = false;
    glm::vec3 position{0.0f}; ///< From the listener (m), in the world's axes: Scene works it out exactly.
    f32 minDistance = 1.0f;   ///< Full volume up to here.
    f32 maxDistance = 50.0f;  ///< Silent from here on (linear fade in between).
};

/// @brief The sound output: plays clips as voices, one listener at the origin.
///
/// Positions are given relative to the listener (Scene subtracts the int64 positions exactly), so
/// sounds stay exact however far out in the world. Without an audio device (or with `silent`), it
/// runs without output: voices still start, advance in time and end, so tests and servers work.
class Audio {
public:
    /// @param silent No output device (headless tools, tests).
    explicit Audio(bool silent = false);
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    /// @brief True when sound reaches a device.
    bool hasOutput() const;

    /// @brief Starts a clip. @return Its voice (0 if the clip is empty or there are too many voices).
    VoiceId play(std::shared_ptr<const AudioClip> clip, const VoiceParams& params = {});
    /// @brief Changes a playing voice (volume, pitch, loop, position...).
    void update(VoiceId voice, const VoiceParams& params);
    /// @brief Stops a voice (and frees it).
    void stop(VoiceId voice);
    /// @brief Stops every voice.
    void stopAll();
    /// @brief True while a voice is playing.
    bool isPlaying(VoiceId voice) const;
    /// @brief How far it has played (seconds).
    f64 position(VoiceId voice) const;
    /// @brief The settings a playing voice has now (default ones if it is not playing).
    VoiceParams voiceParams(VoiceId voice) const;
    /// @brief Playing voices.
    u32 voiceCount() const { return static_cast<u32>(m_voices.size()); }

    /// @brief Turns the listener (it stays at the origin: positions are relative to it).
    void setListenerOrientation(const glm::quat& rotation);
    /// @brief Master volume (0..1+).
    void setMasterVolume(f32 volume);
    f32 masterVolume() const { return m_master; }

    /// @brief Frees the voices that ended; silent outputs also advance by `seconds`. Scene::update calls it.
    void tick(f64 seconds);

    /// @brief At most this many voices play at once.
    static constexpr u32 kMaxVoices = 256;

private:
    struct Voice;
    struct Backend;
    Scope<Backend> m_backend;
    std::unordered_map<u32, Scope<Voice>> m_voices;
    u32 m_next = 1;
    f32 m_master = 1.0f;
};

} // namespace nova
