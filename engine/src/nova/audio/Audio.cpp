// Sound output with miniaudio (its engine: mixing, pitch, 3D panning and attenuation), or silent.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#include <miniaudio.h>

#include "nova/audio/Audio.h"
#include "nova/core/Log.h"

#include <algorithm>
#include <cmath>

namespace nova {

// ------------------------------------------------------------------ decoding --
namespace {
std::shared_ptr<const AudioClip> decodeWith(ma_decoder& decoder, const std::string& name) {
    auto clip = std::make_shared<AudioClip>();
    clip->name = name;
    clip->channels = decoder.outputChannels;
    clip->sampleRate = decoder.outputSampleRate;
    std::vector<f32> chunk(4096 * clip->channels);
    for (;;) {
        ma_uint64 read = 0;
        if (ma_decoder_read_pcm_frames(&decoder, chunk.data(), 4096, &read) != MA_SUCCESS && read == 0) break;
        clip->samples.insert(clip->samples.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read * clip->channels));
        if (read < 4096) break;
    }
    ma_decoder_uninit(&decoder);
    if (clip->samples.empty()) return nullptr;
    return clip;
}
} // namespace

std::shared_ptr<const AudioClip> decodeAudioFile(const std::filesystem::path& path) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0); // keep the file's channels and rate
    ma_decoder decoder;
    if (ma_decoder_init_file_w(path.c_str(), &config, &decoder) != MA_SUCCESS) {
        NOVA_CORE_WARN("Audio: could not decode '{}'", path.string());
        return nullptr;
    }
    return decodeWith(decoder, path.stem().string());
}

std::shared_ptr<const AudioClip> decodeAudioMemory(const void* data, size_t size, const std::string& name) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_memory(data, size, &config, &decoder) != MA_SUCCESS) {
        NOVA_CORE_WARN("Audio: could not decode '{}'", name);
        return nullptr;
    }
    return decodeWith(decoder, name);
}

// ------------------------------------------------------------------- output --
struct Audio::Backend {
    ma_engine engine{};
    bool running = false;
};

struct Audio::Voice {
    std::shared_ptr<const AudioClip> clip; // kept alive while it plays
    VoiceParams params;
    ma_audio_buffer buffer{};
    ma_sound sound{};
    bool device = false; // a miniaudio sound (else silent: `time` advances)
    f64 time = 0.0;
};

Audio::Audio(bool silent) : m_backend(makeScope<Backend>()) {
    if (silent) return;
    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;
    if (ma_engine_init(&config, &m_backend->engine) == MA_SUCCESS) {
        m_backend->running = true;
        NOVA_CORE_INFO("Audio: {} Hz, {} channels", ma_engine_get_sample_rate(&m_backend->engine),
                       ma_engine_get_channels(&m_backend->engine));
    } else {
        NOVA_CORE_WARN("Audio: no output device; sounds play silently");
    }
}

Audio::~Audio() {
    stopAll();
    if (m_backend->running) ma_engine_uninit(&m_backend->engine);
}

bool Audio::hasOutput() const { return m_backend->running; }

namespace {
void apply(ma_sound& s, const VoiceParams& p) {
    ma_sound_set_volume(&s, p.volume);
    ma_sound_set_pitch(&s, std::max(p.pitch, 0.01f));
    ma_sound_set_looping(&s, p.loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_spatialization_enabled(&s, p.spatial ? MA_TRUE : MA_FALSE);
    if (p.spatial) {
        ma_sound_set_positioning(&s, ma_positioning_absolute);
        ma_sound_set_position(&s, p.position.x, p.position.y, p.position.z);
        ma_sound_set_attenuation_model(&s, ma_attenuation_model_linear);
        ma_sound_set_min_distance(&s, std::max(p.minDistance, 0.0f));
        ma_sound_set_max_distance(&s, std::max(p.maxDistance, p.minDistance + 0.001f));
        ma_sound_set_rolloff(&s, 1.0f);
    }
}
} // namespace

VoiceId Audio::play(std::shared_ptr<const AudioClip> clip, const VoiceParams& params) {
    if (!clip || clip->frames() == 0 || m_voices.size() >= kMaxVoices) return {};
    auto voice = makeScope<Voice>();
    voice->clip = std::move(clip);
    voice->params = params;
    if (m_backend->running) {
        ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, voice->clip->channels, voice->clip->frames(),
                                                                voice->clip->samples.data(), nullptr);
        bc.sampleRate = voice->clip->sampleRate;
        if (ma_audio_buffer_init(&bc, &voice->buffer) == MA_SUCCESS) {
            if (ma_sound_init_from_data_source(&m_backend->engine, &voice->buffer, 0, nullptr, &voice->sound) == MA_SUCCESS) {
                voice->device = true;
                apply(voice->sound, params);
                ma_sound_start(&voice->sound);
            } else {
                ma_audio_buffer_uninit(&voice->buffer);
            }
        }
    }
    const VoiceId id{m_next++};
    if (m_next == 0) m_next = 1;
    m_voices.emplace(id.value, std::move(voice));
    return id;
}

void Audio::update(VoiceId id, const VoiceParams& params) {
    auto it = m_voices.find(id.value);
    if (it == m_voices.end()) return;
    it->second->params = params;
    if (it->second->device) apply(it->second->sound, params);
}

void Audio::stop(VoiceId id) {
    auto it = m_voices.find(id.value);
    if (it == m_voices.end()) return;
    Voice& v = *it->second;
    if (v.device) {
        ma_sound_uninit(&v.sound);
        ma_audio_buffer_uninit(&v.buffer);
    }
    m_voices.erase(it);
}

void Audio::stopAll() {
    while (!m_voices.empty()) stop(VoiceId{m_voices.begin()->first});
}

bool Audio::isPlaying(VoiceId id) const {
    auto it = m_voices.find(id.value);
    if (it == m_voices.end()) return false;
    const Voice& v = *it->second;
    if (v.device) return ma_sound_is_playing(&v.sound) && !ma_sound_at_end(&v.sound);
    return v.params.loop || v.time < v.clip->duration();
}

f64 Audio::position(VoiceId id) const {
    auto it = m_voices.find(id.value);
    if (it == m_voices.end()) return 0.0;
    const Voice& v = *it->second;
    if (v.device) {
        float seconds = 0.0f;
        ma_sound_get_cursor_in_seconds(const_cast<ma_sound*>(&v.sound), &seconds);
        return seconds;
    }
    const f64 length = v.clip->duration();
    return v.params.loop && length > 0.0 ? std::fmod(v.time, length) : std::min(v.time, length);
}

VoiceParams Audio::voiceParams(VoiceId id) const {
    auto it = m_voices.find(id.value);
    return it == m_voices.end() ? VoiceParams{} : it->second->params;
}

void Audio::setListenerOrientation(const glm::quat& rotation) {
    if (!m_backend->running) return;
    const glm::vec3 forward = rotation * glm::vec3(0, 0, -1), up = rotation * glm::vec3(0, 1, 0);
    ma_engine_listener_set_position(&m_backend->engine, 0, 0.0f, 0.0f, 0.0f);
    ma_engine_listener_set_direction(&m_backend->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&m_backend->engine, 0, up.x, up.y, up.z);
}

void Audio::setMasterVolume(f32 volume) {
    m_master = std::max(volume, 0.0f);
    if (m_backend->running) ma_engine_set_volume(&m_backend->engine, m_master);
}

void Audio::tick(f64 seconds) {
    std::vector<u32> ended;
    for (auto& [id, v] : m_voices) {
        if (!v->device) v->time += seconds * std::max(v->params.pitch, 0.01f);
        if (!isPlaying(VoiceId{id})) ended.push_back(id);
    }
    for (u32 id : ended) stop(VoiceId{id});
}

} // namespace nova
