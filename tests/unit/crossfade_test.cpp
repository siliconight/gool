// Copyright 2026 Brannen Graves
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing permissions
// and limitations under the License.

// tests/unit/crossfade_test.cpp
//
// Validates:
//   1. Fade-in shape: gain envelope follows sin(t·π/2), not linear.
//   2. Fade-out shape: gain envelope follows cos(t·π/2), not linear.
//   3. MusicChannel.Play crossfades: the new track ramps up while the
//      previous track ramps down over the same duration.
//   4. Equal-power property: when two uncorrelated tracks crossfade
//      with these curves, total RMS stays roughly constant.
//   5. SetEmitterPlaybackSpeed: changing the speed mid-play actually
//      shifts where the cursor is in the source PCM (cheap proxy:
//      observe the rendered fundamental shifts).

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/music_channel.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
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
    const char* Name()    const noexcept override { return "OfflineXfade"; }

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

// Build a constant-amplitude DC source that loops. Lets us read the
// applied gain envelope directly from the rendered output without
// signal-processing artifacts.
std::vector<float> ConstantSource(uint32_t numFrames, float amp) {
    return std::vector<float>(numFrames, amp);
}

std::vector<float> SineSource(uint32_t numFrames, float freq,
                                uint32_t sampleRate, float amp = 0.5f) {
    std::vector<float> v(numFrames);
    for (uint32_t i = 0; i < numFrames; ++i) {
        v[i] = amp * std::sin(2.0f * kPi * freq *
                                static_cast<float>(i) /
                                static_cast<float>(sampleRate));
    }
    return v;
}

// Mean absolute value of one channel of an interleaved stereo block.
// For a constant DC source, this equals the gain.
float MeanAbsLeft(const std::vector<float>& interleaved) {
    if (interleaved.empty()) return 0.0f;
    double s = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < interleaved.size(); i += 2) {
        s += std::abs(interleaved[i]);
        ++n;
    }
    return static_cast<float>(s / static_cast<double>(n));
}

// RMS of one channel of an interleaved stereo block.
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

void TestFadeInShape() {
    std::cout << "  [fade-in: gain follows sin(t·π/2)]\n";
    constexpr uint32_t kSr = 48000;
    audio::AudioRuntime rt;
    {
        audio::AudioConfig cfg;
        cfg.sampleRate = kSr;
        cfg.bufferSize = 192;  // divides 960 (=20ms@48kHz) evenly
        cfg.outputMode = audio::AudioOutputMode::Stereo;
        audio::AudioRuntimeDependencies deps;
        auto bp = std::make_unique<OfflineBackend>();
        OfflineBackend* backend = bp.get();
        deps.backend = std::move(bp);
        const auto rc = rt.Initialize(cfg, std::move(deps));
        assert(rc == audio::AudioResult::Success);

        // Constant 0.5-amplitude DC, 1 second, looping.
        constexpr audio::AudioSoundId kId = 0xFADE0001u;
        const auto src = ConstantSource(kSr, 0.5f);
        rt.RegisterPcmSound(kId,
                             std::span<const float>(src.data(), src.size()),
                             kSr, 1u);

        audio::SoundDefinition def;
        def.soundId      = kId;
        def.spatialized  = false;        // straight to bus, no panning
        def.targetBus    = audio::kBusMaster;
        rt.RegisterSoundDefinition(def);

        // Create emitter with 200 ms fade-in.
        constexpr float kFadeMs = 200.0f;
        audio::EmitterDescriptor desc;
        desc.soundId       = kId;
        desc.isLooping     = true;
        desc.isSpatialized = false;
        desc.fadeInMs      = kFadeMs;
        auto h = rt.CreateEmitter(desc);
        assert(h);

        // Render the first 200 ms in 20 ms chunks; verify gain envelope.
        // Render an extra chunk past the fade so we have a clean
        // steady-state value to normalize against (the mixer's center
        // pan law puts ~0.707 per-channel at full voice gain, so we
        // can't compare to absolute 1.0 here).
        const uint32_t framesPerChunk = kSr / 50;     // 20 ms
        std::vector<float> out;
        std::vector<float> measuredGain;
        for (int chunk = 0; chunk < 11; ++chunk) {
            backend->Render(framesPerChunk, out);
            const float g = MeanAbsLeft(out) / 0.5f;  // normalise by source amp
            measuredGain.push_back(g);
        }
        const float steady = measuredGain.back();
        assert(steady > 0.5f);  // must reach plateau

        // Predicted gain at the END of each 20 ms chunk: the chunk
        // boundary corresponds to t = (chunk+1)/10 of the fade.
        std::cout << "    chunk |  measured |  norm  |  predicted avg\n";
        bool allOk = true;
        for (int i = 0; i < 10; ++i) {
            const float t0 = i / 10.0f;
            const float t1 = (i + 1) / 10.0f;
            // Average of sin(x·π/2) over [t0, t1] is
            //   (cos(t0·π/2) - cos(t1·π/2)) / ((t1-t0)·π/2).
            const float avg = (std::cos(t0 * kPi * 0.5f)
                                - std::cos(t1 * kPi * 0.5f))
                                / ((t1 - t0) * kPi * 0.5f);
            const float norm = measuredGain[i] / steady;
            const float err = std::abs(norm - avg);
            std::printf("    %5d | %8.4f | %.4f | %10.4f (err %.4f)\n",
                         i, measuredGain[i], norm, avg, err);
            if (err > 0.06f) allOk = false;
        }
        if (!allOk) {
            std::cerr << "    FAIL: fade-in envelope does not match sin curve\n";
            std::exit(1);
        }
        rt.DestroyEmitter(h.value());
        rt.Shutdown();
    }
    std::cout << "    OK (envelope matches sin·π/2 to within ±0.04)\n";
}

void TestFadeOutShape() {
    std::cout << "  [fade-out: gain follows cos(t·π/2)]\n";
    constexpr uint32_t kSr = 48000;
    audio::AudioRuntime rt;
    audio::AudioConfig cfg;
    cfg.sampleRate = kSr;
    cfg.bufferSize = 192;  // divides 960 (=20ms@48kHz) evenly
    cfg.outputMode = audio::AudioOutputMode::Stereo;
    audio::AudioRuntimeDependencies deps;
    auto bp = std::make_unique<OfflineBackend>();
    OfflineBackend* backend = bp.get();
    deps.backend = std::move(bp);
    const auto rc = rt.Initialize(cfg, std::move(deps));
    assert(rc == audio::AudioResult::Success);

    constexpr audio::AudioSoundId kId = 0xFADE0002u;
    const auto src = ConstantSource(kSr, 0.5f);
    rt.RegisterPcmSound(kId,
                         std::span<const float>(src.data(), src.size()),
                         kSr, 1u);
    audio::SoundDefinition def;
    def.soundId     = kId;
    def.spatialized = false;
    def.targetBus   = audio::kBusMaster;
    rt.RegisterSoundDefinition(def);

    audio::EmitterDescriptor desc;
    desc.soundId       = kId;
    desc.isLooping     = true;
    desc.isSpatialized = false;
    desc.fadeInMs      = 0.0f;
    auto h = rt.CreateEmitter(desc);
    assert(h);

    // Let it stabilise at full gain (skip 50 ms — past gain smoothing).
    // Steady-state is per-channel; center pan law puts ~0.707 per ear.
    std::vector<float> out;
    backend->Render(kSr / 20, out);
    const float steady = MeanAbsLeft(out) / 0.5f;
    assert(steady > 0.6f);  // ~0.707 expected

    // Now destroy with 200 ms fade-out and measure decay.
    constexpr float kFadeMs = 200.0f;
    rt.DestroyEmitter(h.value(), kFadeMs);

    const uint32_t framesPerChunk = kSr / 50;
    std::vector<float> measured;
    for (int chunk = 0; chunk < 10; ++chunk) {
        backend->Render(framesPerChunk, out);
        measured.push_back(MeanAbsLeft(out) / 0.5f);
    }
    bool allOk = true;
    std::cout << "    chunk |  measured |  norm  |  predicted avg cos\n";
    for (int i = 0; i < 10; ++i) {
        const float t0 = i / 10.0f;
        const float t1 = (i + 1) / 10.0f;
        // Average of cos(x·π/2) over [t0, t1] is
        //   (sin(t1·π/2) - sin(t0·π/2)) / ((t1-t0)·π/2).
        const float avg = (std::sin(t1 * kPi * 0.5f)
                            - std::sin(t0 * kPi * 0.5f))
                            / ((t1 - t0) * kPi * 0.5f);
        const float norm = measured[i] / steady;
        const float err  = std::abs(norm - avg);
        std::printf("    %5d | %8.4f | %.4f | %10.4f (err %.4f)\n",
                     i, measured[i], norm, avg, err);
        if (err > 0.06f) allOk = false;
    }
    if (!allOk) {
        std::cerr << "    FAIL: fade-out envelope does not match cos curve\n";
        std::exit(1);
    }
    rt.Shutdown();
    std::cout << "    OK (envelope matches cos·π/2)\n";
}

void TestMusicChannelCrossfade() {
    std::cout << "  [MusicChannel: A→B crossfade with constant total power]\n";
    constexpr uint32_t kSr = 48000;
    audio::AudioRuntime rt;
    audio::AudioConfig cfg;
    cfg.sampleRate = kSr;
    cfg.bufferSize = 192;  // divides 960 (=20ms@48kHz) evenly
    cfg.outputMode = audio::AudioOutputMode::Stereo;
    audio::AudioRuntimeDependencies deps;
    auto bp = std::make_unique<OfflineBackend>();
    OfflineBackend* backend = bp.get();
    deps.backend = std::move(bp);
    const auto rc = rt.Initialize(cfg, std::move(deps));
    assert(rc == audio::AudioResult::Success);

    // Two uncorrelated sources at the same RMS so a crossfade tests
    // the equal-power property: 200 Hz sine for A, 600 Hz sine for B.
    // RMS of an amp-A sine is A/√2; both have amp=0.5 → RMS≈0.354.
    constexpr audio::AudioSoundId kIdA = 0xFADE000Au;
    constexpr audio::AudioSoundId kIdB = 0xFADE000Bu;
    const auto sa = SineSource(kSr * 5, 200.0f, kSr, 0.5f);  // 5 s loop
    const auto sb = SineSource(kSr * 5, 600.0f, kSr, 0.5f);
    rt.RegisterPcmSound(kIdA, std::span<const float>(sa.data(), sa.size()), kSr, 1u);
    rt.RegisterPcmSound(kIdB, std::span<const float>(sb.data(), sb.size()), kSr, 1u);
    for (auto id : {kIdA, kIdB}) {
        audio::SoundDefinition def;
        def.soundId     = id;
        def.spatialized = false;
        def.targetBus   = audio::kBusMaster;
        rt.RegisterSoundDefinition(def);
    }

    audio::MusicChannel music(rt);
    music.Play(kIdA, /*fadeMs=*/0.0f);  // start A immediately

    // Let A stabilise.
    std::vector<float> out;
    backend->Render(kSr / 20, out);   // 50 ms warm-up
    const float rmsA_alone = RmsLeft(out);
    std::printf("    A alone RMS = %.4f (sine RMS·panL ≈ 0.354·0.707 = 0.250)\n",
                 rmsA_alone);
    // Per-channel center-pan attenuation: 0.354·0.707 ≈ 0.250.
    assert(rmsA_alone > 0.20f && rmsA_alone < 0.30f);

    // Crossfade to B over 200 ms.
    music.Play(kIdB, /*fadeMs=*/200.0f);

    constexpr float kFadeMs = 200.0f;
    constexpr int   kChunks = 10;
    const uint32_t framesPerChunk = static_cast<uint32_t>(
        (kFadeMs / 1000.0f * static_cast<float>(kSr)) / kChunks);
    std::vector<float> rmsThroughCrossfade;
    for (int i = 0; i < kChunks; ++i) {
        backend->Render(framesPerChunk, out);
        rmsThroughCrossfade.push_back(RmsLeft(out));
    }

    // Let B stabilise after fade completes.
    backend->Render(kSr / 20, out);
    const float rmsB_alone = RmsLeft(out);
    std::printf("    B alone RMS = %.4f (expect ≈ 0.250 per channel)\n", rmsB_alone);
    assert(rmsB_alone > 0.20f && rmsB_alone < 0.30f);

    // The RMS during the crossfade should stay within [80%, 120%] of
    // the steady-state RMS. Equal-power for uncorrelated signals
    // says it stays at exactly 100%; the band is wide for chunk
    // averaging and the inevitable correlation between the two
    // signals at certain alignments.
    const float baseline = 0.5f * (rmsA_alone + rmsB_alone);
    bool allOk = true;
    std::cout << "    chunk |  RMS during crossfade  |  ratio vs baseline\n";
    for (int i = 0; i < kChunks; ++i) {
        const float r = rmsThroughCrossfade[i];
        const float ratio = r / baseline;
        std::printf("    %5d |              %.4f  | %.3f\n", i, r, ratio);
        if (ratio < 0.80f || ratio > 1.20f) allOk = false;
    }
    if (!allOk) {
        std::cerr << "    FAIL: crossfade total power dropped or spiked outside [0.80, 1.20] band\n";
        std::exit(1);
    }
    music.Stop(0.0f);
    rt.Shutdown();
    std::cout << "    OK (total power held within ±20% of baseline through 200 ms crossfade)\n";
}

void TestPlaybackSpeed() {
    std::cout << "  [SetEmitterPlaybackSpeed: cursor advances at scaled rate]\n";
    constexpr uint32_t kSr = 48000;
    audio::AudioRuntime rt;
    audio::AudioConfig cfg;
    cfg.sampleRate = kSr;
    cfg.bufferSize = 192;  // divides 960 (=20ms@48kHz) evenly
    cfg.outputMode = audio::AudioOutputMode::Stereo;
    audio::AudioRuntimeDependencies deps;
    auto bp = std::make_unique<OfflineBackend>();
    OfflineBackend* backend = bp.get();
    deps.backend = std::move(bp);
    const auto rc = rt.Initialize(cfg, std::move(deps));
    assert(rc == audio::AudioResult::Success);

    // 440 Hz sine at speed=1.0 should render as 440 Hz; at speed=2.0
    // it renders as 880 Hz. We don't do a real FFT here; instead we
    // count zero-crossings, which is proportional to fundamental
    // frequency for a sine. 440 Hz @ 48 kHz → ~109 zero crossings
    // per 5760-frame (120 ms) window; 880 Hz → ~218.
    constexpr audio::AudioSoundId kId = 0xFADE0003u;
    const auto src = SineSource(kSr * 2, 440.0f, kSr, 0.5f);
    rt.RegisterPcmSound(kId, std::span<const float>(src.data(), src.size()), kSr, 1u);
    audio::SoundDefinition def;
    def.soundId     = kId;
    def.spatialized = false;
    def.targetBus   = audio::kBusMaster;
    rt.RegisterSoundDefinition(def);

    audio::EmitterDescriptor desc;
    desc.soundId       = kId;
    desc.isLooping     = true;
    desc.isSpatialized = false;
    auto h = rt.CreateEmitter(desc);
    assert(h);

    // The UpdateParams pump is gated on having a primary listener.
    // Without one, the smoother values never reach the mixer.
    audio::AudioListener listener;
    listener.position = audio::Vec3{0.0f, 0.0f, 0.0f};
    listener.forward  = audio::Vec3{0.0f, 0.0f, -1.0f};
    listener.velocity = audio::Vec3{};
    rt.SetListener(listener);

    auto countZeroCrossings = [](const std::vector<float>& interleaved) {
        int zc = 0;
        bool prevPos = interleaved.size() > 0 ? interleaved[0] >= 0.0f : true;
        for (size_t i = 2; i < interleaved.size(); i += 2) {  // left ch
            const bool pos = interleaved[i] >= 0.0f;
            if (pos != prevPos) ++zc;
            prevPos = pos;
        }
        return zc;
    };

    // Let it stabilise at speed 1.0.
    std::vector<float> out;
    backend->Render(kSr / 20, out);              // 50 ms warm-up
    backend->Render(kSr * 12 / 100, out);        // 120 ms measurement
    const int zcSpeed1 = countZeroCrossings(out);

    // Bump to speed 2.0 with very small smoothing so we measure the
    // post-ramp value cleanly. 2.0× rate → 880 Hz fundamental.
    // The smoother is pumped on AudioRuntime::Update; without it,
    // the mixer never sees the new pitch target.
    rt.SetEmitterPlaybackSpeed(h.value(), 2.0f, /*smoothingMs=*/5.0f);
    rt.Update(0.020f);                            // 20 ms — past the 5 ms smoothing
    backend->Render(kSr / 20, out);              // 50 ms past the ramp
    rt.Update(0.050f);
    backend->Render(kSr * 12 / 100, out);        // 120 ms measurement
    const int zcSpeed2 = countZeroCrossings(out);

    std::printf("    speed=1.0  zero-crossings/120ms = %d (expect ~106)\n", zcSpeed1);
    std::printf("    speed=2.0  zero-crossings/120ms = %d (expect ~211)\n", zcSpeed2);
    const float ratio = static_cast<float>(zcSpeed2) / static_cast<float>(zcSpeed1);
    std::printf("    ratio = %.3f (expect ≈ 2.0)\n", ratio);
    std::fflush(stdout);
    // Allow ±15 each (block boundaries, partial cycles).
    assert(std::abs(zcSpeed1 - 106) < 15);
    assert(std::abs(zcSpeed2 - 211) < 30);
    // The ratio should be roughly 2.0.
    assert(ratio > 1.7f && ratio < 2.3f);
    rt.DestroyEmitter(h.value());
    rt.Shutdown();
    std::cout << "    OK (2.0× speed doubles fundamental frequency as expected)\n";
}

} // namespace

int main() {
    std::cout << "[crossfade_test] running...\n";
    TestFadeInShape();
    TestFadeOutShape();
    TestMusicChannelCrossfade();
    TestPlaybackSpeed();
    std::cout << "[crossfade_test] OK\n";
    return 0;
}
