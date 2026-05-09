// tests/unit/loop_crossfade_test.cpp
//
// Validates loop-boundary crossfade by synthesizing a sound whose
// last sample is +0.8 and whose first sample is -0.8 — guaranteed
// to click with a naive fmod wrap. We render the loop multiple
// times, sample the boundary region, and compare the
// frame-to-frame delta with crossfade off vs. on.
//
//   - Without crossfade: large delta at every wrap (click).
//   - With crossfade:    smooth transition, delta below threshold.
//
// We also verify that a perfectly-looping sound (same start and
// end value) is unaffected by enabling the crossfade — equal-power
// curves preserve identical content.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <span>
#include <vector>

namespace {

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
    const char* Name()    const noexcept override { return "OfflineLoopXf"; }

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

// Build a test sound that ramps from -0.8 to +0.8 linearly across N frames.
// Sample[0] = -0.8, sample[N-1] = +0.8. Without crossfade, looping
// produces a -1.6 jump at every wrap.
std::vector<float> RampSource(uint32_t numFrames) {
    std::vector<float> v(numFrames);
    for (uint32_t i = 0; i < numFrames; ++i) {
        v[i] = -0.8f + 1.6f * static_cast<float>(i)
                              / static_cast<float>(numFrames - 1);
    }
    return v;
}

// Find the maximum abs frame-to-frame delta in the LEFT channel of
// an interleaved-stereo block. A glitch-free signal has max delta
// equal to the source's per-sample delta plus a small interpolation
// margin; a click produces a delta several times larger.
float MaxAbsDeltaLeft(const std::vector<float>& interleaved) {
    if (interleaved.size() < 4) return 0.0f;
    float maxD = 0.0f;
    float prev = interleaved[0];
    for (size_t i = 2; i < interleaved.size(); i += 2) {
        const float cur = interleaved[i];
        const float d   = std::abs(cur - prev);
        if (d > maxD) maxD = d;
        prev = cur;
    }
    return maxD;
}

// Build, register, create, render a looping sound. Returns the
// max frame-to-frame delta observed in the rendered output.
float MeasureLoopGlitch(const std::vector<float>& src,
                          uint32_t                  sampleRate,
                          float                     loopCrossfadeMs,
                          uint32_t                  framesToRender) {
    audio::AudioRuntime rt;
    audio::AudioConfig  cfg;
    cfg.sampleRate = sampleRate;
    cfg.bufferSize = 192;
    cfg.outputMode = audio::AudioOutputMode::Stereo;
    audio::AudioRuntimeDependencies deps;
    auto bp = std::make_unique<OfflineBackend>();
    OfflineBackend* backend = bp.get();
    deps.backend = std::move(bp);
    const auto rc = rt.Initialize(cfg, std::move(deps));
    assert(rc == audio::AudioResult::Success);

    audio::AudioListener listener;
    rt.SetListener(listener);

    constexpr audio::AudioSoundId kId = 0xC0FE0001u;
    rt.RegisterPcmSound(kId, std::span<const float>(src.data(), src.size()),
                         sampleRate, 1u);
    audio::SoundDefinition def;
    def.soundId          = kId;
    def.spatialized      = false;
    def.targetBus        = audio::kBusMaster;
    def.looping          = true;
    def.loopCrossfadeMs  = loopCrossfadeMs;
    rt.RegisterSoundDefinition(def);

    audio::EmitterDescriptor desc;
    desc.soundId       = kId;
    desc.isLooping     = true;
    desc.isSpatialized = false;
    auto h = rt.CreateEmitter(desc);
    assert(h);

    std::vector<float> out;
    backend->Render(framesToRender, out);

    rt.DestroyEmitter(h.value());
    rt.Shutdown();

    return MaxAbsDeltaLeft(out);
}

void TestClickRemoval() {
    std::cout << "  [click removal: discontinuous loop, off vs. on]\n";
    constexpr uint32_t kSr        = 48000;
    constexpr uint32_t kLoopFrames = 480;          // 10 ms loop
    const auto src = RampSource(kLoopFrames);

    // Render 10 loops — guarantees we cross the boundary 9 times.
    const uint32_t framesToRender = kLoopFrames * 10;

    const float deltaOff = MeasureLoopGlitch(src, kSr, /*xfadeMs=*/0.0f,
                                                framesToRender);
    const float deltaOn  = MeasureLoopGlitch(src, kSr, /*xfadeMs=*/2.0f,
                                                framesToRender);

    // Per-sample delta of the source ramp:
    //   step = 1.6 / (loopFrames - 1) ≈ 0.00334
    // Center pan halves it on the channel: ≈ 0.00236.
    // Without crossfade, the wrap delta is ~1.6 × 0.707 (pan) ≈ 1.13.
    // With a 2 ms (96-frame) crossfade, the worst delta is just the
    // smoothed change, which should be much smaller.
    std::printf("    crossfade off: max |Δ| = %.4f (expect ≈ 1.13 at boundary)\n",
                 deltaOff);
    std::printf("    crossfade on : max |Δ| = %.4f (expect << 0.05)\n",
                 deltaOn);
    std::fflush(stdout);

    if (deltaOff < 0.5f) {
        std::cerr << "    FAIL: even without crossfade, the boundary delta should be huge\n";
        std::exit(1);
    }
    if (deltaOn > 0.10f) {
        std::cerr << "    FAIL: crossfade did not eliminate the click\n";
        std::exit(1);
    }
    if (deltaOn > deltaOff * 0.2f) {
        std::cerr << "    FAIL: crossfade reduction is too small (expect ≥ 5×)\n";
        std::exit(1);
    }
    std::printf("    reduction factor: %.1fx\n", deltaOff / std::max(deltaOn, 1e-6f));
    std::fflush(stdout);
}

void TestPerfectLoopUnchanged() {
    std::cout << "  [perfect loop: enabling crossfade should not change content]\n";
    constexpr uint32_t kSr = 48000;
    constexpr uint32_t kLoopFrames = 4800;        // 100 ms loop

    // 100 Hz sine that completes exactly 10 cycles in the buffer.
    // First and last samples both ≈ 0; perfectly continuous.
    std::vector<float> src(kLoopFrames);
    for (uint32_t i = 0; i < kLoopFrames; ++i) {
        src[i] = 0.5f * std::sin(2.0f * 3.14159265f * 100.0f *
                                  static_cast<float>(i) /
                                  static_cast<float>(kSr));
    }

    const uint32_t framesToRender = kLoopFrames * 3;
    const float deltaOff = MeasureLoopGlitch(src, kSr, 0.0f, framesToRender);
    const float deltaOn  = MeasureLoopGlitch(src, kSr, 5.0f, framesToRender);

    std::printf("    crossfade off: max |Δ| = %.6f\n", deltaOff);
    std::printf("    crossfade on : max |Δ| = %.6f\n", deltaOn);
    std::fflush(stdout);
    // For a clean sine, max sample-to-sample delta is 2π·f·A/sr ≈ 0.0066;
    // both should match within noise.
    const float maxOk = 0.012f;
    if (deltaOff > maxOk || deltaOn > maxOk) {
        std::cerr << "    FAIL: clean sine should have small per-sample delta\n";
        std::exit(1);
    }
    if (std::abs(deltaOn - deltaOff) > 0.005f) {
        std::cerr << "    FAIL: crossfade shouldn't materially affect a perfect loop\n";
        std::exit(1);
    }
}

void TestNonLoopingUnaffected() {
    std::cout << "  [non-looping: loopCrossfadeMs ignored, plays once and stops]\n";
    constexpr uint32_t kSr = 48000;
    constexpr uint32_t kFrames = 2400;
    const auto src = RampSource(kFrames);

    // Set up runtime with a NON-looping emitter. The
    // SoundDefinition has loopCrossfadeMs > 0 but the emitter is
    // not looping — the crossfade should be inert.
    audio::AudioRuntime rt;
    audio::AudioConfig  cfg;
    cfg.sampleRate = kSr;
    cfg.bufferSize = 192;
    cfg.outputMode = audio::AudioOutputMode::Stereo;
    audio::AudioRuntimeDependencies deps;
    auto bp = std::make_unique<OfflineBackend>();
    OfflineBackend* backend = bp.get();
    deps.backend = std::move(bp);
    rt.Initialize(cfg, std::move(deps));

    audio::AudioListener listener;
    rt.SetListener(listener);

    constexpr audio::AudioSoundId kId = 0xC0FE0002u;
    rt.RegisterPcmSound(kId, std::span<const float>(src.data(), src.size()), kSr, 1u);
    audio::SoundDefinition def;
    def.soundId          = kId;
    def.spatialized      = false;
    def.targetBus        = audio::kBusMaster;
    def.looping          = false;
    def.loopCrossfadeMs  = 5.0f;     // intentionally non-zero
    rt.RegisterSoundDefinition(def);

    // For non-looping playback, use SubmitEvent (one-shot).
    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kId,
                     audio::Vec3{0.0f, 0.0f, 0.0f}));

    std::vector<float> out;
    backend->Render(kFrames + 1000, out);  // overshoot

    // After kFrames, output should be silence (one-shot completed).
    // Sum left-channel energy in the LAST 500 frames; should be 0.
    double tail = 0.0;
    int    n    = 0;
    for (size_t i = (out.size() - 1000); i + 1 < out.size(); i += 2) {
        tail += out[i] * out[i];
        ++n;
    }
    const double tailRms = std::sqrt(tail / std::max(1, n));
    std::printf("    tail RMS after one-shot completion: %.6f (expect ≈ 0)\n",
                 tailRms);
    std::fflush(stdout);
    if (tailRms > 0.001) {
        std::cerr << "    FAIL: non-looping sound did not stop\n";
        std::exit(1);
    }
    rt.Shutdown();
}

} // namespace

int main() {
    std::cout << "[loop_crossfade_test] running...\n";
    TestClickRemoval();
    TestPerfectLoopUnchanged();
    TestNonLoopingUnaffected();
    std::cout << "[loop_crossfade_test] OK\n";
    return 0;
}
