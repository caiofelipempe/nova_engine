// Sound in the scene: sources on entities, one-shots, the listener (Scene::updateAudio).
#include "nova/scene/Scene.h"

#include <algorithm>

namespace nova {

namespace {
Audio*& defaultAudio() {
    static Audio* audio = nullptr;
    return audio;
}
Scene::SoundSource& defaultSoundSource() {
    static Scene::SoundSource source;
    return source;
}
} // namespace

void Scene::setDefaultAudio(Audio* audio) { defaultAudio() = audio; }
Audio* Scene::audio() const { return m_audio ? m_audio : defaultAudio(); }
void Scene::setDefaultSoundSource(SoundSource source) { defaultSoundSource() = std::move(source); }

std::shared_ptr<const AudioClip> Scene::findSound(const std::string& clip) const {
    if (clip.empty()) return nullptr;
    if (m_soundSource) return m_soundSource(clip);
    const SoundSource& fallback = defaultSoundSource();
    return fallback ? fallback(clip) : nullptr;
}

// Where sound is heard from: the active listener entity, else the primary camera (where it is drawn).
bool Scene::listenerPose(GlobalPosition& position, glm::quat& rotation) {
    Entity listener;
    for (auto [e, l] : m_registry.view<AudioListenerComponent>().each())
        if (l.active) {
            listener = {e, this};
            break;
        }
    if (!listener.valid()) listener = primaryCamera();
    if (!listener.valid()) return false;
    const TransformComponent pose = renderTransform(listener);
    position = pose.position;
    rotation = pose.orientation();
    return true;
}

namespace {
VoiceParams sourceParams(const AudioSourceComponent& src) {
    VoiceParams p;
    p.volume = src.volume;
    p.pitch = src.pitch;
    p.loop = src.loop;
    p.spatial = src.spatial;
    p.minDistance = src.minDistance;
    p.maxDistance = src.maxDistance;
    return p;
}
} // namespace

void Scene::updateAudio(Duration frame) {
    Audio* out = audio();
    if (!out) return;
    GlobalPosition ear;
    glm::quat turn(1, 0, 0, 0);
    const bool heard = listenerPose(ear, turn);
    out->setListenerOrientation(turn);
    for (auto [e, src] : m_registry.view<AudioSourceComponent>().each()) {
        if (!src.clip.empty() && (src.resolved != src.clip || !src.data)) { // found (again) by its reference
            src.data = findSound(src.clip);
            src.resolved = src.clip;
        }
        if (!src.started && src.playOnStart) {
            src.started = true;
            if (src.data && !src.voice) src.voice = out->play(src.data, sourceParams(src));
        }
        if (!src.voice) continue;
        if (!out->isPlaying(src.voice)) { // ended
            out->stop(src.voice);
            src.voice = {};
            continue;
        }
        VoiceParams p = sourceParams(src);
        if (heard) p.position = (renderTransform(Entity{e, this}).position - ear).toVec3(); // exact, then floats
        out->update(src.voice, p);
    }
    for (PlacedSound& s : m_placedSounds) {
        if (!out->isPlaying(s.voice)) continue;
        if (heard) s.params.position = (s.at - ear).toVec3();
        out->update(s.voice, s.params);
    }
    out->tick(frame.seconds());
    std::erase_if(m_placedSounds, [&](const PlacedSound& s) { return !out->isPlaying(s.voice); });
}

VoiceId Scene::playSound(std::shared_ptr<const AudioClip> clip, const GlobalPosition& at, f32 volume, f32 pitch) {
    Audio* out = audio();
    if (!out || !clip) return {};
    PlacedSound s;
    s.at = at;
    s.params.volume = volume;
    s.params.pitch = pitch;
    s.params.spatial = true;
    GlobalPosition ear;
    glm::quat turn;
    if (listenerPose(ear, turn)) s.params.position = (at - ear).toVec3();
    s.voice = out->play(std::move(clip), s.params);
    if (s.voice) m_placedSounds.push_back(s);
    return s.voice;
}

VoiceId Scene::playSound(std::shared_ptr<const AudioClip> clip, f32 volume, f32 pitch, bool loop) {
    Audio* out = audio();
    if (!out || !clip) return {};
    VoiceParams p;
    p.volume = volume;
    p.pitch = pitch;
    p.loop = loop;
    return out->play(std::move(clip), p);
}

void Scene::playSource(Entity entity) {
    Audio* out = audio();
    if (!out || !entity.valid() || !entity.has<AudioSourceComponent>()) return;
    auto& src = entity.get<AudioSourceComponent>();
    if (!src.clip.empty() && (src.resolved != src.clip || !src.data)) {
        src.data = findSound(src.clip);
        src.resolved = src.clip;
    }
    if (src.voice) out->stop(src.voice);
    src.started = true;
    VoiceParams p = sourceParams(src);
    GlobalPosition ear;
    glm::quat turn;
    if (listenerPose(ear, turn)) p.position = (entity.transform().position - ear).toVec3();
    src.voice = src.data ? out->play(src.data, p) : VoiceId{};
}

void Scene::stopSource(Entity entity) {
    if (!entity.valid() || !entity.has<AudioSourceComponent>()) return;
    auto& src = entity.get<AudioSourceComponent>();
    if (Audio* out = audio(); out && src.voice) out->stop(src.voice);
    src.voice = {};
}

bool Scene::isSourcePlaying(Entity entity) const {
    const Audio* out = audio();
    if (!out || !entity.valid()) return false;
    const auto* src = entity.scene()->registry().try_get<AudioSourceComponent>(entity.handle());
    return src && src->voice && out->isPlaying(src->voice);
}

void Scene::onAudioSourceDestroyed(entt::registry& registry, entt::entity e) {
    auto& src = registry.get<AudioSourceComponent>(e);
    if (Audio* out = audio(); out && src.voice) out->stop(src.voice);
    src.voice = {};
}

} // namespace nova
