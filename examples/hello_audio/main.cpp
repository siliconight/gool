// examples/hello_audio/main.cpp
//
// The smallest demo that produces audible sound. Plays a 440 Hz sine
// for one second through the default audio device, then exits.
//
// Build:
//   cmake -S . -B build
//   cmake --build build --target audio_engine_hello_audio
// Run:
//   ./build/examples/audio_engine_hello_audio
//
// If you hear a steady tone for ~1 second, the engine is working
// end-to-end on your machine: backend opens the device, registers a
// PCM source, mixer renders, output reaches your speakers. From here
// look at examples/ducking, examples/sound_bank, examples/music_crossfade
// for richer integration patterns.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/miniaudio_backend.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

int main() {
    constexpr uint32_t kSampleRate = 48000;
    constexpr float    kPi         = 3.14159265f;

    audio::AudioRuntime rt;
    audio::AudioConfig  cfg;
    cfg.sampleRate = kSampleRate;
    cfg.bufferSize = 512;
    cfg.outputMode = audio::AudioOutputMode::Stereo;

    audio::AudioRuntimeDependencies deps;
    deps.backend = std::make_unique<audio::MiniaudioBackend>();

    if (rt.Initialize(cfg, std::move(deps)) != audio::AudioResult::Success) {
        std::fprintf(stderr, "audio engine init failed (no audio device?)\n");
        return 1;
    }
    audio::AudioListener listener;
    rt.SetListener(listener);

    // Synthesize a 1-second 440 Hz sine.
    constexpr audio::AudioSoundId kId = 0xBEEF0001u;
    std::vector<float> sine(kSampleRate);
    for (uint32_t i = 0; i < kSampleRate; ++i) {
        sine[i] = 0.3f * std::sin(2.0f * kPi * 440.0f *
                                    static_cast<float>(i) /
                                    static_cast<float>(kSampleRate));
    }
    rt.RegisterPcmSound(kId,
                         std::span<const float>(sine.data(), sine.size()),
                         kSampleRate, /*channels=*/1u);

    audio::SoundDefinition def;
    def.soundId     = kId;
    def.spatialized = false;
    def.targetBus   = audio::kBusMaster;
    rt.RegisterSoundDefinition(def);

    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
        kId, audio::Vec3{0.0f, 0.0f, 0.0f}));

    std::printf("hello_audio: playing 440 Hz sine for 1 second...\n");
    std::fflush(stdout);

    // Drive the control thread for 1.2 seconds; the sine plays
    // entirely on the audio backend's render thread.
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1200)) {
        rt.Update(0.020f);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    rt.Shutdown();
    std::printf("done.\n");
    return 0;
}
