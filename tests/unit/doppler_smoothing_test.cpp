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

// tests/unit/doppler_smoothing_test.cpp
//
// Two-layer test of Doppler pitch shift and the per-voice pitch ramp that
// prevents zipper noise when the Doppler ratio updates between render
// blocks.
//
//   1. Mixer-level: drive a Sound voice over a *ramp* source buffer where
//      output[i] = i * 0.001. Because output reads cursor as a float index,
//      the inter-sample slope of the output reveals the resampling step
//      directly. With a ramp source we can read off the actual cursor
//      step without FFT or zero-cross counting:
//
//        slope_i ≈ (output[i+1] - output[i]) / 0.001 ≈ pitch_at_i
//
//      Three sub-tests:
//        * Start at non-unity pitch; no fade-in from 1.0 (initialization)
//        * Pitch jumps mid-stream; first sample of new block is continuous
//          with previous block's slope (smoothing), final sample reaches
//          the new target (convergence within one block)
//        * After one block of ramp, hold target; slope stays at target
//          for subsequent blocks (no drift)
//
//   2. Runtime-level: an emitter with non-zero radial velocity produces an
//      observable pitch shift in the rendered output. The spatializer
//      computes the Doppler ratio from emitter.velocity − listener.velocity
//      projected onto the to-emitter axis; the mixer applies it. We
//      measure the shift with a zero-cross-rate estimate.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/bus.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"

#include "audio_engine/mixer/audio_mixer.h"
#include "audio_engine/mixer/bus_graph.h"
#include "audio_engine/mixer/mixer_command.h"
#include "audio_engine/spatial/default_spatializer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)
#define EXPECT_NEAR(a, b, tol) do { \
    const float _aa = (a), _bb = (b); \
    if (std::fabs(_aa - _bb) > (tol)) { \
        std::fprintf(stderr, "FAIL %s:%d  |%.6f - %.6f| > %.6f\n", \
                     __FILE__, __LINE__, _aa, _bb, (float)(tol)); \
        ++gFails; \
    } \
} while (0)

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kBufferSize = 256;
constexpr float    kPi         = 3.14159265358979323846f;

// -------------------------------------------------------------------------
// Mixer-level: ramp source, observe cursor step via output slope
// -------------------------------------------------------------------------

struct MixerRig {
    BusGraph    bg;
    AudioMixer  mixer;
    std::vector<float> render;

    MixerRig()
        : bg(),
          mixer(/*maxMixVoices*/ 4, /*outputChannels*/ 2,
                /*commandRingDepth*/ 32, &bg, /*sampleRate*/ kSampleRate),
          render(static_cast<size_t>(kBufferSize) * 2, 0.0f)
    {
        BusGraphConfig cfg{};
        if (bg.Build(cfg, kSampleRate, /*channels*/ 2, kBufferSize) != AudioResult::Success)
            ++gFails;
    }

    void StartRampSound(const float* pcm, uint32_t frames, float pitch) {
        MixerCommand cmd;
        cmd.kind        = MixerCommandKind::StartSound;
        cmd.mixSlot     = 1;
        cmd.gain        = 1.0f;
        cmd.pan         = 0.0f;       // center pan: gL = gR = cos(pi/4) ≈ 0.7071
        cmd.pitch       = pitch;
        cmd.targetBus   = kBusMaster;
        cmd.pcmData     = pcm;
        cmd.pcmFrames   = frames;
        cmd.pcmChannels = 1;
        cmd.looping     = false;
        mixer.PostCommand(cmd);
    }

    void UpdatePitch(float pitch) {
        MixerCommand cmd;
        cmd.kind    = MixerCommandKind::UpdateParams;
        cmd.mixSlot = 1;
        cmd.gain    = 1.0f;
        cmd.pan     = 0.0f;
        cmd.pitch   = pitch;
        mixer.PostCommand(cmd);
    }

    // Render N blocks and return the left-channel-only output (every other
    // sample), with the equal-power center-pan gain (~0.7071) divided out so
    // the test reads source values directly.
    std::vector<float> RenderLeftDeganged(uint32_t blocks) {
        constexpr float kInvPan = 1.4142135623730951f; // 1 / cos(pi/4)
        std::vector<float> out;
        out.reserve(static_cast<size_t>(blocks) * kBufferSize);
        for (uint32_t b = 0; b < blocks; ++b) {
            mixer.OnRender(render.data(), kBufferSize, /*channels*/ 2);
            for (uint32_t f = 0; f < kBufferSize; ++f) {
                out.push_back(render[f * 2 + 0] * kInvPan);
            }
        }
        return out;
    }
};

void TestPitchInitNoFadeIn() {
    // Start at pitch=2.0. If smoothing initialised pitchCurrent to 1.0 by
    // mistake, the first block would read at slope 0.001 (unity) and ramp
    // up; the correct behaviour is slope = 0.002 from sample 0.
    MixerRig rig;
    std::vector<float> ramp(2048);
    for (uint32_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) * 0.001f;
    rig.StartRampSound(ramp.data(), static_cast<uint32_t>(ramp.size()), /*pitch*/ 2.0f);
    auto out = rig.RenderLeftDeganged(/*blocks*/ 1);

    // Slope between samples 1 and 2 should already be ~0.002 (target).
    const float slope = out[2] - out[1];
    std::printf("  pitch=2.0 init: slope[1→2] = %.5f (expect ~0.002, tol 5%%)\n", slope);
    EXPECT_NEAR(slope, 0.002f, 0.0001f);
}

void TestPitchSmoothBoundaryAndConverge() {
    // Block 1 at pitch=1.0; block 2 at pitch=3.0 (target jump).
    // Expectations:
    //   * end-of-block-1 slope ≈ 0.001 (steady)
    //   * start-of-block-2 slope ≈ 0.001 (continuous with block 1)
    //   * end-of-block-2 slope ≈ 0.003 (target reached within one block)
    //   * end-of-block-3 slope ≈ 0.003 (target held, no drift)
    MixerRig rig;
    std::vector<float> ramp(4096);
    for (uint32_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) * 0.001f;
    rig.StartRampSound(ramp.data(), static_cast<uint32_t>(ramp.size()), /*pitch*/ 1.0f);

    auto block1 = rig.RenderLeftDeganged(1);
    rig.UpdatePitch(3.0f);
    auto block2 = rig.RenderLeftDeganged(1);
    auto block3 = rig.RenderLeftDeganged(1);

    const uint32_t N = kBufferSize;
    const float slopeEnd1   = block1[N - 1] - block1[N - 2];
    const float slopeStart2 = block2[1]     - block2[0];
    const float slopeEnd2   = block2[N - 1] - block2[N - 2];
    const float slopeEnd3   = block3[N - 1] - block3[N - 2];

    std::printf("  ramp pitch 1.0→3.0: slopes  end1=%.4f  start2=%.4f  end2=%.4f  end3=%.4f\n",
                slopeEnd1, slopeStart2, slopeEnd2, slopeEnd3);

    EXPECT_NEAR(slopeEnd1,   0.001f, 0.0002f);   // baseline
    EXPECT_NEAR(slopeStart2, 0.001f, 0.0005f);   // continuity at boundary
    EXPECT_NEAR(slopeEnd2,   0.003f, 0.0005f);   // converged to target
    EXPECT_NEAR(slopeEnd3,   0.003f, 0.0002f);   // held; no drift
}

void TestPitchTargetReachableHigh() {
    // A drastic pitch shift (1.0 → 0.5, i.e. pitch DOWN; Doppler-receding
    // case at high speed) still converges within one block. Note: with
    // integer-cast cursor and a 0.001-step ramp source, instantaneous
    // sample-to-sample slopes alternate between 0 and 0.001 when pitch is
    // fractional, so we average over a window to read off the *effective*
    // step rate.
    MixerRig rig;
    std::vector<float> ramp(4096);
    for (uint32_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) * 0.001f;
    rig.StartRampSound(ramp.data(), static_cast<uint32_t>(ramp.size()), /*pitch*/ 1.0f);

    rig.RenderLeftDeganged(1);
    rig.UpdatePitch(0.5f);
    auto block2 = rig.RenderLeftDeganged(1);

    const uint32_t N = kBufferSize;
    // Average slope over the last 32 samples; by then pitchCurrent has
    // converged to 0.5, and 32 samples gives ~16 integer-boundary crossings
    // averaged out.
    constexpr uint32_t kWin = 32;
    const float avgSlopeEnd = (block2[N - 1] - block2[N - 1 - kWin]) / kWin;
    std::printf("  ramp pitch 1.0→0.5: avg slope last %u = %.5f (expect ~0.0005)\n",
                kWin, avgSlopeEnd);
    EXPECT_NEAR(avgSlopeEnd, 0.0005f, 0.0001f);
}

// -------------------------------------------------------------------------
// Spatializer-level: verify Doppler pitch math directly. Combined with the
// mixer-level ramp tests above, this proves the chain from emitter velocity
// → spatializer pitch ratio → mixer pitchCurrent ramp → cursor step.
// -------------------------------------------------------------------------

void TestSpatializerDopplerMath() {
    DefaultSpatializer sp;
    SpatialEmitterView e{};
    e.position    = {5.0f, 0.0f, 0.0f};
    e.spatialized = true;
    e.minDistance = 1.0f;
    e.maxDistance = 100.0f;
    e.volumeFloor = 1.0f;     // disable distance attenuation for the test

    SpatialListenerView lis{};
    lis.position = {0.0f, 0.0f, 0.0f};
    lis.velocity = {0.0f, 0.0f, 0.0f};
    lis.forward  = {0.0f, 0.0f, -1.0f};
    lis.up       = {0.0f, 1.0f, 0.0f};

    SpatialEnvironmentState env{};
    env.dopplerEnabled = true;
    env.speedOfSound   = 343.0f;

    // Static source: pitch ratio = 1.0
    e.velocity = {0.0f, 0.0f, 0.0f};
    auto p0 = sp.Calculate(e, lis, env);
    std::printf("  Doppler pitch:  static = %.4f  (expect 1.0000)\n", p0.pitch);
    EXPECT_NEAR(p0.pitch, 1.0f, 0.001f);

    // Receding at +30 m/s along +X (away from listener): pitch < 1
    e.velocity = {30.0f, 0.0f, 0.0f};
    auto p1 = sp.Calculate(e, lis, env);
    const float expectRecede = 343.0f / (343.0f + 30.0f);     // 0.9196
    std::printf("  Doppler pitch: receding = %.4f  (expect %.4f)\n", p1.pitch, expectRecede);
    EXPECT_NEAR(p1.pitch, expectRecede, 0.001f);

    // Approaching at -30 m/s along +X (toward listener): pitch > 1
    e.velocity = {-30.0f, 0.0f, 0.0f};
    auto p2 = sp.Calculate(e, lis, env);
    const float expectApproach = 343.0f / (343.0f - 30.0f);    // 1.0958
    std::printf("  Doppler pitch: approach = %.4f  (expect %.4f)\n", p2.pitch, expectApproach);
    EXPECT_NEAR(p2.pitch, expectApproach, 0.001f);

    // Doppler disabled: pitch always 1.0 regardless of velocity.
    env.dopplerEnabled = false;
    auto p3 = sp.Calculate(e, lis, env);
    std::printf("  Doppler pitch: disabled = %.4f  (expect 1.0000)\n", p3.pitch);
    EXPECT_NEAR(p3.pitch, 1.0f, 0.001f);

    // Clamp behaviour: an absurdly fast approaching emitter must clamp at 2.0
    // and not blow up to infinity (the formula's denominator approaches zero).
    env.dopplerEnabled = true;
    e.velocity = {-1000.0f, 0.0f, 0.0f};
    auto p4 = sp.Calculate(e, lis, env);
    std::printf("  Doppler pitch: clamp+   = %.4f  (expect 2.0000)\n", p4.pitch);
    EXPECT_NEAR(p4.pitch, 2.0f, 0.001f);

    // Tangential motion (perpendicular to the listener-to-emitter axis) has
    // zero radial component, so pitch is unshifted even at high speed.
    e.velocity = {0.0f, 50.0f, 0.0f};
    auto p5 = sp.Calculate(e, lis, env);
    std::printf("  Doppler pitch: tangent  = %.4f  (expect 1.0000)\n", p5.pitch);
    EXPECT_NEAR(p5.pitch, 1.0f, 0.001f);
}

// -------------------------------------------------------------------------
// Interpolation quality. Drive the mixer with a 1 kHz sine sampled at 48 kHz
// and play it back at pitch=1.5. Output should be a clean 1.5 kHz sine. Compare
// to a reference 1.5 kHz sine generated directly. RMS error reveals the
// interpolation quality: nearest-neighbour resampling at pitch=1.5 produces
// audible aliasing (RMS error in the 0.05–0.10 range relative to a 0.707 RMS
// signal); linear interpolation drops it well below 0.01.
// -------------------------------------------------------------------------

void TestSineInterpolationQualityAtNonUnityPitch() {
    constexpr float kSrcHz   = 1000.0f;
    constexpr float kPitch   = 1.5f;
    constexpr float kOutHz   = kSrcHz * kPitch;          // 1500 Hz
    constexpr uint32_t kSrcN = kSampleRate;               // 1 s of source
    constexpr uint32_t kRenderBlocks = 32;                // ~ 170 ms

    // Source: 1 kHz sine at engine rate.
    std::vector<float> srcPcm(kSrcN);
    for (uint32_t i = 0; i < kSrcN; ++i) {
        srcPcm[i] = std::sin(2.0f * kPi * kSrcHz * static_cast<float>(i)
                                       / static_cast<float>(kSampleRate));
    }

    MixerRig rig;
    rig.StartRampSound(srcPcm.data(), kSrcN, kPitch);
    auto out = rig.RenderLeftDeganged(kRenderBlocks);

    // Skip the first block so the pitch ramp (which doesn't apply here since
    // we started at the target pitch) and any startup transients are gone.
    const size_t skip = kBufferSize;
    if (out.size() <= skip + 100) { ++gFails; return; }
    const size_t n = out.size() - skip;

    // Reference: 1.5 kHz sine at engine rate, phase-aligned. The mixer reads
    // src[0] at output frame 0, then src[1.5] at output frame 1, etc.
    // src[t * pitch] = sin(2π * srcHz * (t * pitch) / sr) = sin(2π * outHz * t / sr).
    std::vector<float> ref(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(skip + i) / static_cast<float>(kSampleRate);
        ref[i] = std::sin(2.0f * kPi * kOutHz * t);
    }

    double err2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const float e = out[skip + i] - ref[i];
        err2 += static_cast<double>(e) * e;
    }
    const float rmsErr = static_cast<float>(std::sqrt(err2 / static_cast<double>(n)));
    // Reference RMS is 1/sqrt(2) ≈ 0.7071. Linear interpolation introduces a
    // small HF rolloff (sinc-shaped response); for 1.5 kHz at 48 kHz that's
    // negligible. Tight bound proves we're not running nearest-neighbour.
    std::printf("  pitch=1.5 sine resample: rms_err=%.5f (linear-interp expect <0.01)\n",
                rmsErr);
    EXPECT(rmsErr < 0.01f);
}

// -------------------------------------------------------------------------
// Runtime-level end-to-end. An emitter with non-zero radial velocity must
// produce a measurable pitch shift in the rendered output; proving the
// chain spatializer.pitch → UpdateParams → mixer pitchCurrent → cursor step
// → resampled audio composes correctly through AudioRuntime's Update.
//
// We synthesise a 1 kHz sine, register it as a looping spatialised emitter
// approaching the listener at 50 m/s, and count zero crossings in the
// rendered output. Expected pitch = 343 / (343 - 50) ≈ 1.171, so output
// should land near 1170 Hz. Then we flip the velocity sign (receding) and
// confirm the zero-cross rate drops below 1 kHz.
// -------------------------------------------------------------------------

class DopplerOfflineBackend final : public IAudioBackend {
public:
    AudioResult Start(const AudioBackendConfig& cfg, IAudioRenderCallback* cb) override {
        cfg_ = cfg; cb_ = cb;
        scratch_.assign(static_cast<size_t>(cfg.bufferSize) * cfg.channels, 0.0f);
        return AudioResult::Success;
    }
    void Stop() override { cb_ = nullptr; }
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "DopplerOffline"; }
    void Render(uint32_t frames, std::vector<float>& out) {
        out.clear();
        if (!cb_) return;
        const uint32_t bs = cfg_.bufferSize, ch = cfg_.channels;
        out.reserve(static_cast<size_t>(frames + bs) * ch);
        uint32_t produced = 0;
        while (produced < frames) {
            cb_->OnRender(scratch_.data(), bs, ch);
            const uint32_t take = std::min(bs, frames - produced);
            out.insert(out.end(), scratch_.begin(), scratch_.begin() + take * ch);
            produced += take;
        }
    }
private:
    AudioBackendConfig    cfg_{};
    IAudioRenderCallback* cb_ = nullptr;
    std::vector<float>    scratch_;
};

uint32_t CountZeroCrossingsLeft(const std::vector<float>& interleavedStereo) {
    uint32_t crossings = 0;
    if (interleavedStereo.size() < 4) return 0;
    float prev = interleavedStereo[0];
    for (size_t i = 2; i < interleavedStereo.size(); i += 2) {
        const float cur = interleavedStereo[i];
        if ((prev <= 0.0f && cur > 0.0f) || (prev > 0.0f && cur <= 0.0f)) ++crossings;
        prev = cur;
    }
    return crossings;
}

float MeasureRuntimeFrequency(const Vec3& emitterVelocity) {
    AudioConfig cfg;
    cfg.sampleRate                   = kSampleRate;
    cfg.bufferSize                   = kBufferSize;
    cfg.outputMode                   = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters     = 4;
    cfg.budget.maxRegisteredSounds   = 4;
    cfg.budget.maxStreamingAssets    = 1;
    cfg.budget.maxStreamingVoices    = 1;
    cfg.budget.maxVoiceSources       = 0;
    cfg.enableDoppler                = true;
    cfg.speedOfSound                 = 343.0f;

    AudioRuntime rt;
    auto backend = std::make_unique<DopplerOfflineBackend>();
    DopplerOfflineBackend* bp = backend.get();

    AudioRuntimeDependencies deps;
    deps.backend = std::move(backend);
    if (rt.Initialize(cfg, std::move(deps)) != AudioResult::Success) {
        ++gFails;
        return 0.0f;
    }

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    rt.SetListener(lis);

    constexpr AudioSoundId kSnd = 1;
    std::vector<float> srcPcm(kSampleRate);
    for (uint32_t i = 0; i < kSampleRate; ++i) {
        srcPcm[i] = std::sin(2.0f * kPi * 1000.0f * static_cast<float>(i)
                                          / static_cast<float>(kSampleRate));
    }
    rt.RegisterPcmSound(kSnd, srcPcm, kSampleRate, 1);

    SoundDefinition def;
    def.soundId      = kSnd;
    def.category     = AudioCategory::SFX;
    def.targetBus    = kBusMaster;
    def.spatialized  = true;
    def.looping      = true;
    def.attenuation.minDistance = 1.0f;
    def.attenuation.maxDistance = 1000.0f;
    def.attenuation.volumeFloor = 0.5f;
    rt.RegisterSoundDefinition(def);

    EmitterDescriptor ed;
    ed.soundId          = kSnd;
    ed.position         = {10.0f, 0.0f, 0.0f};       // 10 m down +X axis
    ed.velocity         = emitterVelocity;
    ed.targetBus        = kBusMaster;
    ed.category         = AudioCategory::SFX;
    ed.isLooping        = true;
    ed.isSpatialized    = true;
    ed.priority         = AudioPriority::Normal;
    ed.attenuation      = def.attenuation;
    auto h = rt.CreateEmitter(ed);
    if (!h) { ++gFails; return 0.0f; }

    // Settle: a few ticks for ramps and smoothing.
    std::vector<float> out;
    for (int i = 0; i < 10; ++i) {
        rt.Update(0.025f);
        bp->Render(kSampleRate / 40, out);   // 25 ms / tick
    }
    // Measure: 250 ms of audio.
    std::vector<float> meas;
    for (int i = 0; i < 10; ++i) {
        rt.Update(0.025f);
        bp->Render(kSampleRate / 40, out);
        meas.insert(meas.end(), out.begin(), out.end());
    }

    rt.DestroyEmitter(h.value());
    rt.Shutdown();

    const uint32_t crossings = CountZeroCrossingsLeft(meas);
    const float    seconds   = static_cast<float>(meas.size() / 2)
                                / static_cast<float>(kSampleRate);
    return static_cast<float>(crossings) / 2.0f / seconds;
}

void TestRuntimeDopplerShiftsAudibly() {
    // Approaching at 50 m/s along -X (toward listener at origin).
    const float fApproach = MeasureRuntimeFrequency(Vec3{-50.0f, 0.0f, 0.0f});
    // Receding at 50 m/s along +X.
    const float fRecede   = MeasureRuntimeFrequency(Vec3{50.0f, 0.0f, 0.0f});

    const float expectApproach = 1000.0f * (343.0f / (343.0f - 50.0f));    // ~1170.7 Hz
    const float expectRecede   = 1000.0f * (343.0f / (343.0f + 50.0f));    // ~872.6 Hz

    std::printf("  approaching @ 50 m/s: f=%.1f Hz (expect ~%.1f)\n", fApproach, expectApproach);
    std::printf("  receding    @ 50 m/s: f=%.1f Hz (expect ~%.1f)\n", fRecede,   expectRecede);

    // ±5 % to absorb resampler quantization + any control-thread smoothing.
    EXPECT(std::fabs(fApproach - expectApproach) < expectApproach * 0.05f);
    EXPECT(std::fabs(fRecede   - expectRecede)   < expectRecede   * 0.05f);
    EXPECT(fApproach > fRecede);                                 // qualitative direction
}

} // namespace

int main() {
    std::printf("[doppler_smoothing_test] running...\n");
    TestPitchInitNoFadeIn();
    TestPitchSmoothBoundaryAndConverge();
    TestPitchTargetReachableHigh();
    TestSpatializerDopplerMath();
    TestSineInterpolationQualityAtNonUnityPitch();
    TestRuntimeDopplerShiftsAudibly();
    if (gFails == 0) { std::printf("[doppler_smoothing_test] OK\n"); return 0; }
    std::fprintf(stderr, "[doppler_smoothing_test] %d failure(s)\n", gFails);
    return 1;
}
