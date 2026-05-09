// examples/minimal/main.cpp
//
// End-to-end smoke test. Builds an AudioRuntime with the default
// (Null) backend, registers a synthesized sine-wave sound, places a
// listener, fires a few PlaySoundAtLocation events plus a long-lived
// looping emitter, ticks the runtime for ~250 ms, and prints stats.
//
// Output is silent (NullAudioBackend discards the mixed buffer); this is
// a wiring/integration check, not a hearing test.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "audio_engine/audio_runtime.h"

using namespace audio;

namespace {

std::vector<float> MakeSine(float freqHz, float seconds, uint32_t sampleRate) {
    const uint32_t frames = static_cast<uint32_t>(seconds * static_cast<float>(sampleRate));
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
    config.sampleRate            = 48000;
    config.bufferSize            = 256;
    config.outputMode            = AudioOutputMode::Stereo;
    config.budget.maxActiveEmitters = 32;
    config.budget.maxVoiceSources   = 4;

    if (auto rc = runtime.Initialize(config); rc != AudioResult::Success) {
        std::fprintf(stderr, "Initialize failed: %s\n", ToString(rc));
        return 1;
    }

    // Register two sounds.
    constexpr AudioSoundId kBeep  = 1001;
    constexpr AudioSoundId kDrone = 1002;

    {
        SoundDefinition def;
        def.soundId      = kBeep;
        def.category     = AudioCategory::SFX;
        def.attenuation.minDistance = 1.0f;
        def.attenuation.maxDistance = 30.0f;
        runtime.RegisterSoundDefinition(def);

        const auto pcm = MakeSine(880.0f, 0.15f, config.sampleRate);
        runtime.RegisterPcmSound(kBeep, pcm, config.sampleRate, /*channels=*/1);
    }
    {
        SoundDefinition def;
        def.soundId      = kDrone;
        def.category     = AudioCategory::Ambience;
        def.looping      = true;
        def.attenuation.minDistance = 2.0f;
        def.attenuation.maxDistance = 60.0f;
        runtime.RegisterSoundDefinition(def);

        const auto pcm = MakeSine(220.0f, 1.0f, config.sampleRate);
        runtime.RegisterPcmSound(kDrone, pcm, config.sampleRate, /*channels=*/1);
    }

    // Listener at origin facing -Z.
    AudioListener L;
    L.playerId = 1;
    L.position = Vec3{0.0f, 0.0f, 0.0f};
    L.forward  = Vec3{0.0f, 0.0f, -1.0f};
    L.up       = Vec3{0.0f, 1.0f, 0.0f};
    runtime.SetListener(L);

    // Looping drone to the right.
    EmitterDescriptor droneDesc;
    droneDesc.soundId   = kDrone;
    droneDesc.position  = Vec3{5.0f, 0.0f, 0.0f};
    droneDesc.isLooping = true;
    droneDesc.category  = AudioCategory::Ambience;
    droneDesc.attenuation.minDistance = 2.0f;
    droneDesc.attenuation.maxDistance = 60.0f;
    auto droneH = runtime.CreateEmitter(droneDesc);
    if (!droneH) {
        std::fprintf(stderr, "CreateEmitter failed: %s\n", ToString(droneH.error()));
        return 1;
    }
    std::printf("Created drone emitter: idx=%u gen=%u\n",
                 droneH.value().index, droneH.value().generation);

    // Fire a couple of one-shots over time.
    runtime.SubmitEvent(AudioEvent::MakePlaySoundAtLocation(
        kBeep, Vec3{-3.0f, 0.0f, -1.0f}));

    // Tick for ~250 ms.
    constexpr int kTicks      = 25;
    constexpr float kTickSec  = 0.01f;     // 10 ms per tick
    for (int i = 0; i < kTicks; ++i) {
        runtime.Update(kTickSec);

        if (i == 5) {
            runtime.SubmitEvent(AudioEvent::MakePlaySoundAtLocation(
                kBeep, Vec3{2.0f, 0.0f, -2.0f}));
        }
        if (i == 10) {
            // Smoothly ramp the drone gain down.
            runtime.SetEmitterParameter(droneH.value(), AudioParameterIds::Gain, 0.3f, 100.0f);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(
            static_cast<int>(kTickSec * 1e6f)));
    }

    auto stats = runtime.GetStats();
    std::printf("---- stats ----\n");
    std::printf("active emitters:        %u\n", stats.activeEmitters);
    std::printf("active voice sources:   %u\n", stats.activeVoiceSources);
    std::printf("events drained:         %u\n", stats.eventsDrainedLastTick);
    std::printf("late events discarded:  %u\n", stats.lateEventsDiscardedLastTick);
    std::printf("occlusion checks:       %u\n", stats.occlusionChecksLastTick);
    std::printf("mixer voices active:    %u\n", stats.mixerVoicesActive);
    std::printf("render callbacks total: %llu\n",
                 static_cast<unsigned long long>(stats.totalRenderCallbacks));
    std::printf("render underruns:       %llu\n",
                 static_cast<unsigned long long>(stats.renderUnderruns));

    runtime.DestroyEmitter(droneH.value());
    runtime.Shutdown();
    return 0;
}
