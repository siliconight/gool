// examples/streaming/main.cpp
//
// End-to-end demonstration of the streaming voice path. Builds a long
// procedural music buffer (10 seconds of stacked sines at 32 kHz to also
// exercise the resampler; the engine runs at 48 kHz), registers it as a
// streaming sound via RegisterStreamingSoundFromMemory, plays it through
// a single emitter, and renders 1.5 seconds of output via a synchronous
// offline backend. The control thread tops up the streaming asset's float
// ring each tick; the mixer reads from that ring like it does for voice
// chat.
//
// Why "from memory" rather than "from file"? Files require a real decoder
// (dr_wav / stb_vorbis / dr_flac) which are off by default. The memory
// path uses a synthetic in-engine decoder so this example runs in a fresh
// checkout with no network access. Once you've enabled
// AUDIO_ENGINE_DECODERS_OGG=ON (and have stb_vorbis.c either vendored or
// fetched), substitute RegisterStreamingSoundFromFile for the same
// behaviour against an .ogg / .wav / .flac file.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/bus.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace audio;

namespace {

constexpr AudioSoundId kSndMusic = 100;

// Synchronous offline backend; same trick as the ducking example. Stashes
// the callback, lets the main thread drive Render(N) deterministically.
class OfflineBackend final : public IAudioBackend {
public:
    AudioResult Start(const AudioBackendConfig& cfg, IAudioRenderCallback* cb) override {
        cfg_      = cfg;
        callback_ = cb;
        scratch_.assign(static_cast<size_t>(cfg.bufferSize) * cfg.channels, 0.0f);
        return AudioResult::Success;
    }
    void Stop() override { callback_ = nullptr; }
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "Offline"; }

    void Render(uint32_t frames, std::vector<float>& out) {
        out.clear();
        if (!callback_) return;
        const uint32_t bs = cfg_.bufferSize;
        const uint32_t ch = cfg_.channels;
        out.reserve(static_cast<size_t>(frames + bs) * ch);
        uint32_t produced = 0;
        while (produced < frames) {
            callback_->OnRender(scratch_.data(), bs, ch);
            const uint32_t take = std::min(bs, frames - produced);
            out.insert(out.end(),
                       scratch_.begin(),
                       scratch_.begin() + take * ch);
            produced += take;
        }
    }

private:
    AudioBackendConfig    cfg_{};
    IAudioRenderCallback* callback_ = nullptr;
    std::vector<float>    scratch_;
};

std::vector<float> MakeProceduralMusic(uint32_t sampleRate, uint32_t seconds) {
    // A simple stacked-sine pad. Mono. Source rate is intentionally not 48k
    // so the registry's automatic resampling kicks in.
    const uint32_t frames = sampleRate * seconds;
    std::vector<float> out(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
        const float a = std::sin(2.0f * 3.14159265f * 220.0f * t);
        const float b = std::sin(2.0f * 3.14159265f * 330.0f * t);
        out[i] = 0.4f * (a + b) * 0.5f;
    }
    return out;
}

float Rms(const float* data, size_t samples) {
    if (samples == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < samples; ++i) acc += static_cast<double>(data[i]) * data[i];
    return static_cast<float>(std::sqrt(acc / static_cast<double>(samples)));
}

float DbFromAmp(float a) { return a > 1e-10f ? 20.0f * std::log10(a) : -120.0f; }

} // namespace

int main() {
    constexpr uint32_t kEngineRate    = 48000;
    constexpr uint32_t kBufferSize    = 256;
    constexpr uint32_t kSourceRate    = 32000;     // forces the resampler
    constexpr uint32_t kSourceSeconds = 10;

    AudioConfig cfg;
    cfg.sampleRate = kEngineRate;
    cfg.bufferSize = kBufferSize;
    cfg.outputMode = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters     = 16;
    cfg.budget.maxRegisteredSounds   = 8;
    cfg.budget.maxStreamingAssets    = 4;
    cfg.budget.maxStreamingVoices    = 2;
    cfg.budget.maxVoiceSources       = 0;
    // Default streamingRingFrames (24000 ≈ 500 ms at 48 kHz) is fine.

    auto backend = std::make_unique<OfflineBackend>();
    OfflineBackend* bp = backend.get();

    AudioRuntime rt;
    AudioRuntimeDependencies deps;
    deps.backend = std::move(backend);
    if (rt.Initialize(cfg, std::move(deps)) != AudioResult::Success) {
        std::fprintf(stderr, "Initialize failed\n");
        return 1;
    }

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    rt.SetListener(lis);

    // ---- Register streaming asset -----------------------------------------
    auto music = MakeProceduralMusic(kSourceRate, kSourceSeconds);
    std::printf("Procedural music: %u frames @ %u Hz mono (%u s of audio)\n",
                static_cast<unsigned>(music.size()),
                static_cast<unsigned>(kSourceRate),
                static_cast<unsigned>(kSourceSeconds));

    SoundDefinition def;
    def.soundId     = kSndMusic;
    def.category    = AudioCategory::Music;
    def.targetBus   = kBusMaster;
    def.spatialized = false;
    def.looping     = true;
    rt.RegisterSoundDefinition(def);

    if (auto rc = rt.RegisterStreamingSoundFromMemory(
            kSndMusic, std::move(music), kSourceRate, /*channels=*/1);
        rc != AudioResult::Success) {
        std::fprintf(stderr, "RegisterStreamingSoundFromMemory failed (rc=%d)\n", static_cast<int>(rc));
        return 1;
    }

    EmitterDescriptor ed;
    ed.soundId       = kSndMusic;
    ed.targetBus     = kBusMaster;
    ed.category      = AudioCategory::Music;
    ed.isLooping     = true;
    ed.isSpatialized = false;
    auto h = rt.CreateEmitter(ed);
    if (!h) {
        std::fprintf(stderr, "CreateEmitter failed\n");
        return 1;
    }

    // ---- Drive the engine -------------------------------------------------
    // 25 ms ticks for 1.5 s. Each tick: control-thread Update (drains command
    // ring, pumps streaming rings), then the offline backend renders 25 ms
    // of audio synchronously and we measure RMS / dB.
    constexpr uint32_t kTickMs        = 25;
    const     uint32_t kFramesPerTick = (kEngineRate * kTickMs) / 1000;
    constexpr uint32_t kTicks         = 60;     // 1.5 s

    std::vector<float> buf;
    buf.reserve(static_cast<size_t>(kFramesPerTick + kBufferSize) * 2);

    std::printf("\nStreaming via control-thread pump (%u ticks @ %u ms):\n",
                kTicks, kTickMs);
    for (uint32_t i = 0; i < kTicks; ++i) {
        rt.Update(static_cast<float>(kTickMs) / 1000.0f);
        bp->Render(kFramesPerTick, buf);

        if (i < 4 || i % 8 == 0 || i == kTicks - 1) {
            const float rms = Rms(buf.data(), buf.size());
            std::printf("  t=%4u ms   rms=%.4f  (%6.2f dB)\n",
                         i * kTickMs, rms, DbFromAmp(rms));
        }
    }

    rt.DestroyEmitter(h.value());
    rt.Shutdown();

    std::printf(
        "\nThe RMS column is non-zero throughout; the streaming pump kept\n"
        "the per-asset float ring topped up while the mixer read from it,\n"
        "and the resampler converted 32 kHz source to the 48 kHz engine\n"
        "rate. Replace RegisterStreamingSoundFromMemory with\n"
        "RegisterStreamingSoundFromFile (and AUDIO_ENGINE_DECODERS_OGG=ON)\n"
        "to play a real .ogg / .wav / .flac file through the same path.\n");
    return 0;
}
