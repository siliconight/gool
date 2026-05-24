// examples/music_crossfade/main.cpp
//
// Demonstrates:
//   1. MusicChannel: crossfading between two music tracks with
//      equal-power (cos/sin) curves, so total power stays constant.
//   2. SetEmitterPlaybackSpeed: changing playback rate at runtime
//      with built-in smoothing (no clicks).
//
// Uses synthetic sine tones at different frequencies for the two
// "tracks" so you can hear the crossover in offline measurement.
// Renders to stdout as a sequence of RMS measurements; no audio
// device required.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/music_channel.h"

#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

namespace {

constexpr float kPi = 3.14159265f;

class OfflineBackend final : public audio::IAudioBackend {
public:
    audio::AudioResult Start(const audio::AudioBackendConfig& cfg,
                              audio::IAudioRenderCallback*    cb) override {
        cfg_ = cfg; cb_ = cb;
        scratch_.assign(static_cast<size_t>(cfg.bufferSize) * cfg.channels, 0.0f);
        return audio::AudioResult::Success;
    }
    void     Stop() override { cb_ = nullptr; }
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "OfflineXfadeExample"; }

    void Render(uint32_t frames, std::vector<float>& out) {
        out.clear();
        if (!cb_) return;
        const uint32_t bs = cfg_.bufferSize, ch = cfg_.channels;
        out.reserve(static_cast<size_t>(frames + bs) * ch);
        uint32_t produced = 0;
        while (produced < frames) {
            cb_->OnRender(scratch_.data(), bs, ch);
            const uint32_t take = std::min(bs, frames - produced);
            out.insert(out.end(),
                        scratch_.begin(),
                        scratch_.begin() + take * ch);
            produced += take;
        }
    }
private:
    audio::AudioBackendConfig         cfg_{};
    audio::IAudioRenderCallback*      cb_ = nullptr;
    std::vector<float>                scratch_;
};

std::vector<float> Sine(uint32_t numFrames, float freq, uint32_t sampleRate, float amp) {
    std::vector<float> v(numFrames);
    for (uint32_t i = 0; i < numFrames; ++i) {
        v[i] = amp * std::sin(2.0f * kPi * freq *
                                static_cast<float>(i) /
                                static_cast<float>(sampleRate));
    }
    return v;
}

float RmsLeft(const std::vector<float>& interleaved) {
    if (interleaved.empty()) return 0.0f;
    double s = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < interleaved.size(); i += 2) {
        s += interleaved[i] * interleaved[i];
        ++n;
    }
    return static_cast<float>(std::sqrt(s / static_cast<double>(n)));
}

int CountZeroCrossingsLeft(const std::vector<float>& interleaved) {
    if (interleaved.size() < 4) return 0;
    int zc = 0;
    bool prev = interleaved[0] >= 0.0f;
    for (size_t i = 2; i < interleaved.size(); i += 2) {
        const bool pos = interleaved[i] >= 0.0f;
        if (pos != prev) ++zc;
        prev = pos;
    }
    return zc;
}

} // namespace

int main() {
    constexpr uint32_t kSr = 48000;

    audio::AudioRuntime rt;
    audio::AudioConfig  cfg;
    cfg.sampleRate = kSr;
    cfg.bufferSize = 192;
    cfg.outputMode = audio::AudioOutputMode::Stereo;
    audio::AudioRuntimeDependencies deps;
    auto bp = std::make_unique<OfflineBackend>();
    OfflineBackend* backend = bp.get();
    deps.backend = std::move(bp);
    if (rt.Initialize(cfg, std::move(deps)) != audio::AudioResult::Success) {
        std::fprintf(stderr, "init failed\n");
        return 1;
    }

    // The UpdateParams pump (which delivers parameter changes from
    // the smoother to the mixer) only runs when there's a primary
    // listener. For non-spatialized music, the listener position
    // doesn't matter.
    audio::AudioListener listener;
    rt.SetListener(listener);

    // Register two synthetic looping "tracks": a 200 Hz tone for A
    // and a 600 Hz tone for B. Sound IDs 0xAAAA and 0xBBBB.
    constexpr audio::AudioSoundId kIdA = 0xAAAAu;
    constexpr audio::AudioSoundId kIdB = 0xBBBBu;
    const auto sa = Sine(kSr * 2, 200.0f, kSr, 0.5f);  // 2-second loop
    const auto sb = Sine(kSr * 2, 600.0f, kSr, 0.5f);
    rt.RegisterPcmSound(kIdA, std::span<const float>(sa.data(), sa.size()), kSr, 1u);
    rt.RegisterPcmSound(kIdB, std::span<const float>(sb.data(), sb.size()), kSr, 1u);
    for (auto id : {kIdA, kIdB}) {
        audio::SoundDefinition def;
        def.soundId     = id;
        def.spatialized = false;
        def.targetBus   = audio::kBusMaster;
        rt.RegisterSoundDefinition(def);
    }

    // ----- Phase 1: start track A, play it for 50 ms ---------------
    audio::MusicChannel music(rt);
    music.Play(kIdA, /*fadeMs=*/0.0f);

    std::vector<float> out;
    backend->Render(kSr / 20, out);  // 50 ms warm-up
    std::printf("phase 1: track A alone\n");
    std::printf("  RMS = %.4f, zero-crossings/50ms = %d (≈%.0f Hz)\n",
                 RmsLeft(out),
                 CountZeroCrossingsLeft(out),
                 CountZeroCrossingsLeft(out) * 10.0);  // 50ms → ×20 → ÷2

    // ----- Phase 2: 300 ms crossfade A → B -------------------------
    std::printf("\nphase 2: crossfade A → B over 300 ms\n");
    music.Play(kIdB, /*fadeMs=*/300.0f);

    constexpr int   kSteps = 6;
    const uint32_t  framesPerStep = (kSr * 300 / 1000) / kSteps;  // 50 ms each
    std::printf("  step | RMS    | zc/50ms\n");
    for (int i = 0; i < kSteps; ++i) {
        backend->Render(framesPerStep, out);
        std::printf("  %4d | %.4f |    %3d\n",
                     i, RmsLeft(out), CountZeroCrossingsLeft(out));
    }

    // ----- Phase 3: B alone, then double playback speed ------------
    std::printf("\nphase 3: track B alone\n");
    backend->Render(kSr / 20, out);  // 50 ms past fade
    const int zcBSpeed1 = CountZeroCrossingsLeft(out);
    std::printf("  RMS = %.4f, zero-crossings/50ms = %d (≈%.0f Hz at speed 1.0)\n",
                 RmsLeft(out), zcBSpeed1, zcBSpeed1 * 10.0);

    std::printf("\nphase 4: bump B's playback speed to 1.5×\n");
    rt.SetEmitterPlaybackSpeed(music.Current(), 1.5f, /*smoothingMs=*/30.0f);
    rt.Update(0.040f);              // pump the smoother past the ramp
    backend->Render(kSr / 20, out);  // 50 ms post-ramp
    rt.Update(0.050f);
    backend->Render(kSr / 20, out);  // 50 ms measurement
    const int zcBSpeed15 = CountZeroCrossingsLeft(out);
    std::printf("  RMS = %.4f, zero-crossings/50ms = %d (≈%.0f Hz at speed 1.5)\n",
                 RmsLeft(out), zcBSpeed15, zcBSpeed15 * 10.0);
    std::printf("  ratio vs speed 1.0: %.3f (expect ≈ 1.5)\n",
                 static_cast<float>(zcBSpeed15) / static_cast<float>(zcBSpeed1));

    // ----- Phase 5: stop with 200 ms fade-out ----------------------
    std::printf("\nphase 5: stop with 200 ms fade-out\n");
    music.Stop(200.0f);
    constexpr int kStopSteps = 5;
    const uint32_t stopStepFrames = (kSr * 200 / 1000) / kStopSteps;
    std::printf("  step | RMS\n");
    for (int i = 0; i < kStopSteps; ++i) {
        backend->Render(stopStepFrames, out);
        std::printf("  %4d | %.4f\n", i, RmsLeft(out));
    }

    rt.Shutdown();
    std::printf("\ndone.\n");
    return 0;
}
