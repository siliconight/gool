// examples/playback/main.cpp
//
// Audible end-to-end demo. Spins up the runtime with the miniaudio backend,
// registers a 1-second 440 Hz sine wave, places a listener facing -Z, and
// retriggers a one-shot at three positions (left, center, right) to make
// the spatial pan obvious. Total runtime: ~5 seconds, then clean shutdown.
//
// Build (CMake):
//     cmake -S . -B build -DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON
//     cmake --build build -j
//     ./build/examples/playback/audio_engine_playback
//
// If you don't hear anything: check `audio.MiniaudioBackend opened: ...`
// in the console output, confirm system volume, and make sure the runtime's
// configured sample rate matches what the device negotiated (the example
// re-reads SampleRate() after Start() and prints it).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend/miniaudio_backend.h"

using namespace audio;

namespace {

std::vector<float> MakeSine(float freqHz, float seconds, uint32_t sampleRate) {
    const auto frames = static_cast<uint32_t>(seconds * static_cast<float>(sampleRate));
    std::vector<float> v(frames);
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float dphi = kTwoPi * freqHz / static_cast<float>(sampleRate);
    float phi = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        v[i] = 0.25f * std::sin(phi);
        phi += dphi;
        if (phi > kTwoPi) phi -= kTwoPi;
    }
    return v;
}

} // namespace

int main() {
    AudioRuntime runtime;

    AudioConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 480;            // 10 ms at 48 kHz
    config.outputMode = AudioOutputMode::Stereo;
    config.budget.maxActiveEmitters = 16;
    config.budget.maxVoiceSources   = 4;

    AudioRuntimeDependencies deps;
    deps.backend = std::make_unique<MiniaudioBackend>();
    // Other seams (spatializer, geometry, voice codec) fall back to the
    // engine's built-in defaults when left null.

    if (auto rc = runtime.Initialize(config, std::move(deps));
        rc != AudioResult::Success) {
        std::fprintf(stderr, "Initialize failed: %s\n", ToString(rc));
        return 1;
    }

    // Register a 1-second 440 Hz sine wave as PCM.
    constexpr AudioSoundId kBeep = 2001;
    {
        SoundDefinition def;
        def.soundId  = kBeep;
        def.category = AudioCategory::SFX;
        def.attenuation.minDistance = 1.0f;
        def.attenuation.maxDistance = 30.0f;
        runtime.RegisterSoundDefinition(def);

        const auto pcm = MakeSine(440.0f, 1.0f, config.sampleRate);
        runtime.RegisterPcmSound(kBeep, pcm, config.sampleRate, /*channels=*/1);
    }

    // Listener at origin facing -Z (camera looking down -Z, +X to the right).
    AudioListener L;
    L.playerId = 1;
    L.position = Vec3{0.0f, 0.0f, 0.0f};
    L.forward  = Vec3{0.0f, 0.0f, -1.0f};
    L.up       = Vec3{0.0f, 1.0f, 0.0f};
    runtime.SetListener(L);

    std::printf("audio_engine playback example\n");
    std::printf("  scheduled tones at -X, center, +X (you should hear them pan)\n");

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    // Drive the runtime in 10 ms ticks for ~5 seconds. Trigger a tone at
    // t=0.5s on the left, t=2.0s in the center, t=3.5s on the right.
    bool fired_left = false, fired_center = false, fired_right = false;
    constexpr float kTickSec = 0.010f;

    while (true) {
        const auto now    = clock::now();
        const float tSec  = std::chrono::duration<float>(now - start).count();
        if (tSec > 5.0f) break;

        if (!fired_left && tSec >= 0.5f) {
            runtime.SubmitEvent(AudioEvent::MakePlaySoundAtLocation(
                kBeep, Vec3{-3.0f, 0.0f, -1.0f}));
            fired_left = true;
            std::printf("  [t=%.2fs] tone @ (-3, 0, -1)  (left)\n", tSec);
        }
        if (!fired_center && tSec >= 2.0f) {
            runtime.SubmitEvent(AudioEvent::MakePlaySoundAtLocation(
                kBeep, Vec3{0.0f, 0.0f, -1.0f}));
            fired_center = true;
            std::printf("  [t=%.2fs] tone @ ( 0, 0, -1)  (center)\n", tSec);
        }
        if (!fired_right && tSec >= 3.5f) {
            runtime.SubmitEvent(AudioEvent::MakePlaySoundAtLocation(
                kBeep, Vec3{3.0f, 0.0f, -1.0f}));
            fired_right = true;
            std::printf("  [t=%.2fs] tone @ (+3, 0, -1)  (right)\n", tSec);
        }

        runtime.Update(kTickSec);
        std::this_thread::sleep_for(std::chrono::microseconds(
            static_cast<int>(kTickSec * 1e6f)));
    }

    auto stats = runtime.GetStats();
    std::printf("---- stats ----\n");
    std::printf("active emitters:        %u\n", stats.activeEmitters);
    std::printf("events drained:         %u\n", stats.eventsDrainedLastTick);
    std::printf("mixer voices active:    %u\n", stats.mixerVoicesActive);
    std::printf("render callbacks total: %llu\n",
                 static_cast<unsigned long long>(stats.totalRenderCallbacks));
    std::printf("render underruns:       %llu\n",
                 static_cast<unsigned long long>(stats.renderUnderruns));

    runtime.Shutdown();
    return 0;
}
