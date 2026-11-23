// Headless tests of sound: decoding, the silent output's voices, and the scene's sources,
// one-shots and listener (positions relative to it, exact far out and across seams), sound assets.
#include <nova/Nova.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

using namespace nova;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAILED: %s (%s:%d)\n", expr, file, line);
    }
}
#define CHECK(x) check((x), #x, __FILE__, __LINE__)

// A 16-bit PCM WAV file: `seconds` of a 440 Hz tone, mono.
void writeWav(const fs::path& path, u32 rate, f64 seconds) {
    const u32 frames = static_cast<u32>(rate * seconds);
    std::vector<int16_t> pcm(frames);
    for (u32 i = 0; i < frames; ++i) pcm[i] = static_cast<int16_t>(std::sin(i * 2.0 * 3.14159265 * 440.0 / rate) * 16000.0);
    std::ofstream f(path, std::ios::binary);
    auto u32le = [&](u32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16le = [&](u16 v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const u32 bytes = frames * 2;
    f.write("RIFF", 4);
    u32le(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32le(16);
    u16le(1); // PCM
    u16le(1); // mono
    u32le(rate);
    u32le(rate * 2);
    u16le(2);
    u16le(16);
    f.write("data", 4);
    u32le(bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), bytes);
}

std::shared_ptr<AudioClip> tone(f64 seconds) {
    auto clip = std::make_shared<AudioClip>();
    clip->name = "tone";
    clip->sampleRate = 1000;
    clip->samples.assign(static_cast<size_t>(seconds * 1000), 0.25f);
    return clip;
}

fs::path tempDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / ("nova_audio_" + std::string(name));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void decoding() {
    const fs::path dir = tempDir("decode");
    writeWav(dir / "beep.wav", 8000, 0.5);
    auto clip = decodeAudioFile(dir / "beep.wav");
    CHECK(clip && clip->channels == 1 && clip->sampleRate == 8000 && clip->frames() == 4000);
    CHECK(clip && std::abs(clip->duration() - 0.5) < 1e-9 && clip->name == "beep");
    f32 peak = 0.0f;
    if (clip)
        for (f32 v : clip->samples) peak = std::max(peak, std::abs(v));
    CHECK(std::abs(peak - 16000.0f / 32768.0f) < 0.01f); // the samples as floats
    CHECK(!decodeAudioFile(dir / "missing.wav"));
    fs::remove_all(dir);
}

void silentVoices() {
    // Without a device, voices still play out in time: they start, advance and end.
    Audio audio(true);
    CHECK(!audio.hasOutput());
    const VoiceId v = audio.play(tone(0.5));
    CHECK(v && audio.isPlaying(v) && audio.voiceCount() == 1);
    audio.tick(0.3);
    CHECK(audio.isPlaying(v) && std::abs(audio.position(v) - 0.3) < 1e-9);
    audio.tick(0.3);
    CHECK(!audio.isPlaying(v) && audio.voiceCount() == 0); // ended, freed
    VoiceParams loud;
    loud.loop = true;
    loud.pitch = 2.0f;
    const VoiceId looped = audio.play(tone(0.5), loud);
    audio.tick(0.4); // twice as fast: 0.8 s of the clip, wrapped
    CHECK(audio.isPlaying(looped) && std::abs(audio.position(looped) - 0.3) < 1e-9);
    audio.stop(looped);
    CHECK(audio.voiceCount() == 0 && !audio.play(nullptr) && !audio.play(std::make_shared<AudioClip>()));
}

void sceneSounds() {
    Audio audio(true);
    // Far out in the world: positions relative to the listener stay exact.
    const f64 far = 3.0e6;
    Scene scene;
    scene.setAudio(&audio);
    Entity cam = scene.createEntity("Camera");
    cam.add<CameraComponent>();
    cam.transform().position = {far, 0, 0};
    Entity radio = scene.createEntity("Radio");
    radio.transform().position = {far + 3.25, 1.5, -2.0};
    auto& src = radio.add<AudioSourceComponent>();
    src.data = tone(10.0);
    src.loop = true;
    scene.update(Duration::perSecond(60)); // plays on start
    CHECK(scene.isSourcePlaying(radio) && audio.voiceCount() == 1);
    const VoiceParams p = audio.voiceParams(src.voice);
    CHECK(p.spatial && p.loop && glm::length(p.position - glm::vec3(3.25f, 1.5f, -2.0f)) < 1e-5f);
    // The listener moves: the sound is heard from where it is now.
    cam.transform().position = {far + 10.0, 0, 0};
    scene.update(Duration::perSecond(60));
    CHECK(glm::length(audio.voiceParams(src.voice).position - glm::vec3(-6.75f, 1.5f, -2.0f)) < 1e-5f);
    // A listener entity wins over the camera.
    Entity ear = scene.createEntity("Ear");
    ear.add<AudioListenerComponent>();
    ear.transform().position = {far + 3.25, 1.5, 0.0};
    scene.update(Duration::perSecond(60));
    CHECK(glm::length(audio.voiceParams(src.voice).position - glm::vec3(0.0f, 0.0f, -2.0f)) < 1e-5f);
    // Settings change while it plays; stop and play again.
    src.volume = 0.5f;
    scene.update(Duration::perSecond(60));
    CHECK(audio.voiceParams(src.voice).volume == 0.5f);
    scene.stopSource(radio);
    CHECK(!scene.isSourcePlaying(radio) && audio.voiceCount() == 0);
    scene.playSource(radio);
    CHECK(scene.isSourcePlaying(radio));
    // Destroying the entity stops its sound.
    scene.destroyEntity(radio);
    CHECK(audio.voiceCount() == 0);

    // A one-shot at a place stays there while the listener moves, and ends by itself.
    const VoiceId bang = scene.playSound(tone(0.1), GlobalPosition(far + 3.25, 1.5, 5.0));
    CHECK(bang && glm::length(audio.voiceParams(bang).position - glm::vec3(0, 0, 5)) < 1e-5f);
    ear.transform().position = {far + 3.25, 1.5, 1.0};
    scene.update(Duration::perSecond(60));
    CHECK(glm::length(audio.voiceParams(bang).position - glm::vec3(0, 0, 4)) < 1e-5f);
    for (int i = 0; i < 12; ++i) scene.update(Duration::perSecond(60));
    CHECK(!audio.isPlaying(bang) && audio.voiceCount() == 0);
    // Heard flat (music): not placed.
    const VoiceId music = scene.playSound(tone(1.0), 0.8f, 1.0f, true);
    CHECK(music && !audio.voiceParams(music).spatial);
    audio.stopAll();

    // Across a seam of a small world: the shortest way.
    Scene small;
    small.setAudio(&audio);
    small.setWorldSize(WorldSize::cube(20.0));
    Entity c2 = small.createEntity("Camera");
    c2.add<CameraComponent>();
    c2.transform().position = {9, 0, 0};
    Entity s2 = small.createEntity("Bell");
    s2.transform().position = {-9, 0, 0}; // 2 m past the seam
    s2.add<AudioSourceComponent>().data = tone(5.0);
    small.update(Duration::perSecond(60));
    CHECK(glm::length(audio.voiceParams(s2.get<AudioSourceComponent>().voice).position - glm::vec3(2, 0, 0)) < 1e-5f);
    WorldSize::setActive({});
}

void soundAssets() {
    // A sound file is an asset: placed in a scene it is an entity with a source that finds its
    // clip again after saving (by reference, through the scene's sound source).
    const fs::path root = tempDir("assets");
    writeWav(root / "chime.wav", 8000, 0.25);
    AssetDatabase db;
    db.setRoot(root);
    const AssetRecord* r = db.findByPath("chime.wav");
    CHECK(r && r->type == "Sound");
    if (!r) return;
    Audio audio(true);
    Scene scene;
    scene.setAudio(&audio);
    scene.setSoundSource(db.soundSource());
    auto made = db.instantiate(r->guid, scene, GlobalPosition(1, 0, 0));
    CHECK(made.size() == 1 && made[0].has<AudioSourceComponent>());
    const auto& src = made[0].get<AudioSourceComponent>();
    CHECK(src.clip == AssetDatabase::makeRef(r->guid, 0) && src.data && src.data->frames() == 2000);
    const std::string json = serializeScene(scene);
    Scene back;
    back.setAudio(&audio);
    back.setSoundSource(db.soundSource());
    CHECK(deserializeScene(back, json));
    Entity again = back.findEntity("chime");
    CHECK(again.valid() && again.get<AudioSourceComponent>().clip == src.clip && !again.get<AudioSourceComponent>().data);
    back.update(Duration::perSecond(60));
    CHECK(again.get<AudioSourceComponent>().data && back.isSourcePlaying(again));
    fs::remove_all(root);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Log::init();
    Log::core().set_level(spdlog::level::err);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"decoding a WAV file", decoding},
        {"silent output: voices start, advance, loop and end", silentVoices},
        {"scene: sources, listener, one-shots, far out and across seams", sceneSounds},
        {"sound assets: placed, saved by reference, found again", soundAssets},
    };
    for (auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    Log::shutdown();
    return g_failures == 0 ? 0 : 1;
}
