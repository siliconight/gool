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

// tests/unit/compressor_test.cpp
//
// Sanity-checks CompressorEffect at the DSP level, independent of the bus
// graph / mixer / runtime. Drives the effect directly with synthesized
// signals.
//
// Pre-Tier-A coverage (v0.7 and earlier):
//   1. With no sidechain and a quiet input, gain reduction stays at 0 dB.
//   2. With self-sidechain on a loud input, the envelope rises above
//      threshold and the compressor reduces gain.
//   3. With explicit sidechain (loud) on a quiet main input, the main
//      output is attenuated (the ducking case).
//   4. After a loud sidechain pulse ends, gain reduction releases.
//
// Tier A coverage (v0.8.0):
//   5. Soft knee: kneeWidthDb > 0 produces measurable reduction at exactly
//      threshold, while hard knee (kneeWidthDb = 0) does not.
//   6. Mix ratio: mix=0.5 produces output level midway between dry and
//      fully-compressed.
//   7. Range cap: extreme input is bounded to maxReductionDb.
//   8. Sidechain HPF: low-frequency sidechain content above the cutoff
//      passes through and triggers compression; below-cutoff content
//      does not.
//   9. Hold: gain reduction stays elevated for holdMs after the
//      sidechain drops, then releases normally.
//  10. RMS vs Peak: a single-sample transient produces less reduction in
//      RMS mode than in Peak mode (RMS averages it out, Peak doesn't).

#include "audio_engine/dsp/compressor.h"
#include "audio_engine/bus.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels   = 1;

std::vector<float> ConstantBuffer(uint32_t frames, float amplitude) {
    return std::vector<float>(static_cast<size_t>(frames) * kChannels, amplitude);
}

// Mono sine generator at hzHz, frames samples, amplitude amp.
std::vector<float> SineBuffer(uint32_t frames, float hz, float amp) {
    std::vector<float> out(frames);
    const float w = 2.0f * 3.14159265358979323846f * hz / static_cast<float>(kSampleRate);
    for (uint32_t i = 0; i < frames; ++i) {
        out[i] = amp * std::sin(w * static_cast<float>(i));
    }
    return out;
}

// Helper: produce a baseline CompressorConfig with the same legacy
// parameters used by the pre-Tier-A tests. Tier A defaults intact.
CompressorConfig LegacyConfig(float thr, float ratio, float attack,
                                 float release, float makeup, BusId scBus) {
    CompressorConfig cc;
    cc.thresholdDb  = thr;
    cc.ratio        = ratio;
    cc.attackMs     = attack;
    cc.releaseMs    = release;
    cc.makeupDb     = makeup;
    cc.sidechainBus = scBus;
    return cc;
}

// =============================================================================
// Pre-Tier-A regression tests (migrated to CompressorConfig).
// =============================================================================

void TestNoSidechainQuietSignal() {
    auto cc = LegacyConfig(/*thr*/-40.0f, /*ratio*/8.0f, /*atk*/5.0f,
                              /*rel*/100.0f, /*mkp*/0.0f, kInvalidBusId);
    CompressorEffect c(cc);
    c.Prepare(kSampleRate, kChannels);

    auto buf = ConstantBuffer(2048, 0.001f);     // -60 dB, well below threshold
    c.Process(buf.data(), 2048, kChannels, nullptr, 0);

    EXPECT(c.CurrentReductionDb() < 0.5f);
}

void TestSelfSidechainLoudSignal() {
    auto cc = LegacyConfig(-20.0f, 8.0f, 5.0f, 100.0f, 0.0f, kInvalidBusId);
    CompressorEffect c(cc);
    c.Prepare(kSampleRate, kChannels);

    auto buf = ConstantBuffer(static_cast<uint32_t>(kSampleRate * 0.1), 0.5f);
    c.Process(buf.data(), static_cast<uint32_t>(buf.size()), kChannels, nullptr, 0);

    EXPECT(c.CurrentEnvelopeDb() > -20.0f);
    EXPECT(c.CurrentReductionDb() > 5.0f);

    const float lastSample = buf.back();
    EXPECT(std::abs(lastSample) < 0.5f);
}

void TestSidechainDucksQuietMain() {
    auto cc = LegacyConfig(-20.0f, 8.0f, 5.0f, 100.0f, 0.0f, /*sc bus stub*/42);
    CompressorEffect c(cc);
    c.Prepare(kSampleRate, kChannels);

    const uint32_t frames = static_cast<uint32_t>(kSampleRate * 0.1);
    auto main = ConstantBuffer(frames, 0.0316f);     // -30 dB approx
    auto side = ConstantBuffer(frames, 1.0f);         // 0 dB

    const float mainBefore = std::abs(main.front());
    c.Process(main.data(), frames, kChannels, side.data(), kChannels);
    const float mainAfter = std::abs(main.back());

    EXPECT(c.CurrentEnvelopeDb() > -1.0f);
    EXPECT(c.CurrentReductionDb() > 10.0f);
    EXPECT(mainAfter < mainBefore * 0.5f);
}

void TestSidechainReleaseRecovers() {
    auto cc = LegacyConfig(-20.0f, 8.0f, 5.0f, 50.0f, 0.0f, 42);
    CompressorEffect c(cc);
    c.Prepare(kSampleRate, kChannels);

    const uint32_t pulseFrames = static_cast<uint32_t>(kSampleRate * 0.05);
    const uint32_t restFrames  = static_cast<uint32_t>(kSampleRate * 0.5);

    auto mainPulse = ConstantBuffer(pulseFrames, 0.1f);
    auto sidePulse = ConstantBuffer(pulseFrames, 1.0f);
    c.Process(mainPulse.data(), pulseFrames, kChannels, sidePulse.data(), kChannels);
    const float reductionDuring = c.CurrentReductionDb();

    auto mainRest = ConstantBuffer(restFrames, 0.1f);
    auto sideRest = ConstantBuffer(restFrames, 0.0f);
    c.Process(mainRest.data(), restFrames, kChannels, sideRest.data(), kChannels);
    const float reductionAfter = c.CurrentReductionDb();

    EXPECT(reductionDuring > 5.0f);
    EXPECT(reductionAfter < 1.0f);
}

// =============================================================================
// Tier A sub-tests.
// =============================================================================

// Test 5. Soft knee makes a measurable difference exactly at threshold.
//   Hard knee (kneeWidthDb=0): no reduction at threshold (only above).
//   Soft knee (kneeWidthDb=6): partial reduction at threshold (knee is
//   centered on threshold, so x = threshold sits at the midpoint of the
//   transition curve).
void TestSoftKneeMeasurableTransition() {
    // Input at exactly -20 dB (threshold).
    const float kAtThreshold = 0.1f;  // ≈ -20 dB
    const uint32_t frames = static_cast<uint32_t>(kSampleRate * 0.1);

    // Hard knee — should produce ≤ tiny reduction (envelope sits AT
    // threshold, gain computer's hard-knee branch produces 0 reduction).
    {
        auto cc = LegacyConfig(-20.0f, 8.0f, 5.0f, 100.0f, 0.0f, kInvalidBusId);
        cc.kneeWidthDb = 0.0f;
        CompressorEffect c(cc);
        c.Prepare(kSampleRate, kChannels);
        auto buf = ConstantBuffer(frames, kAtThreshold);
        c.Process(buf.data(), frames, kChannels, nullptr, 0);
        // Hard knee: at threshold exactly, reduction should be near zero.
        EXPECT(c.CurrentReductionDb() < 0.5f);
    }

    // Soft knee — center of knee is at threshold; reduction should be
    // measurable (~half of full-band reduction).
    {
        auto cc = LegacyConfig(-20.0f, 8.0f, 5.0f, 100.0f, 0.0f, kInvalidBusId);
        cc.kneeWidthDb = 6.0f;       // ±3 dB around threshold
        CompressorEffect c(cc);
        c.Prepare(kSampleRate, kChannels);
        auto buf = ConstantBuffer(frames, kAtThreshold);
        c.Process(buf.data(), frames, kChannels, nullptr, 0);
        // Soft knee at threshold: should produce measurable but small
        // reduction (the quadratic curve is at its half-amplitude point).
        const float r = c.CurrentReductionDb();
        EXPECT(r > 0.5f);
        EXPECT(r < 5.0f);  // not full-strength reduction
    }
}

// Test 6. Mix ratio: mix=0.0 is dry, mix=1.0 is fully compressed,
// mix=0.5 sits between them in measurable terms.
void TestMixRatioBlend() {
    const uint32_t frames = static_cast<uint32_t>(kSampleRate * 0.1);
    const float kInput = 0.5f;

    auto runWith = [&](float mix) -> float {
        auto cc = LegacyConfig(-20.0f, 8.0f, 5.0f, 100.0f, 0.0f, kInvalidBusId);
        cc.mixRatio = mix;
        CompressorEffect c(cc);
        c.Prepare(kSampleRate, kChannels);
        auto buf = ConstantBuffer(frames, kInput);
        c.Process(buf.data(), frames, kChannels, nullptr, 0);
        return std::abs(buf.back());
    };

    const float dryOnly  = runWith(0.0f);   // ≈ 0.5 (passthrough)
    const float halfMix  = runWith(0.5f);
    const float fullWet  = runWith(1.0f);

    EXPECT(std::abs(dryOnly - kInput) < 0.01f);    // passthrough
    EXPECT(fullWet < dryOnly);                      // compressed
    // Half-mix sits between dry and wet (allowing for envelope time-
    // alignment slop).
    EXPECT(halfMix > fullWet);
    EXPECT(halfMix < dryOnly);
}

// Test 7. maxReductionDb caps the reduction even with absurd input.
void TestMaxReductionCap() {
    auto cc = LegacyConfig(-40.0f, 100.0f, 1.0f, 100.0f, 0.0f, kInvalidBusId);
    cc.maxReductionDb = 3.0f;       // hard cap at 3 dB
    CompressorEffect c(cc);
    c.Prepare(kSampleRate, kChannels);

    // Loud input — would push reduction far above 3 dB without the cap
    // (threshold -40, ratio 100:1, input at 0 dB ≈ 40 dB above threshold,
    // would produce ~39 dB reduction).
    const uint32_t frames = static_cast<uint32_t>(kSampleRate * 0.1);
    auto buf = ConstantBuffer(frames, 1.0f);
    c.Process(buf.data(), frames, kChannels, nullptr, 0);

    // Reduction should be exactly at the cap (within a small float tol
    // for the envelope follower's settling).
    EXPECT(c.CurrentReductionDb() <= 3.05f);
    EXPECT(c.CurrentReductionDb() >  2.5f);   // hit the cap, not below it
}

// Test 8. Sidechain HPF: 60 Hz content in the sidechain doesn't trigger
// compression when HPF cutoff is set to 200 Hz; the same content WITHOUT
// the HPF does trigger.
void TestSidechainHpfFilters() {
    const uint32_t frames = static_cast<uint32_t>(kSampleRate * 0.2);
    auto mainBuf  = ConstantBuffer(frames, 0.05f);  // quiet main
    auto sineSide = SineBuffer(frames, 60.0f, 0.9f); // loud 60 Hz sidechain

    // No HPF: 60 Hz energy passes through detector, triggers compression.
    {
        auto cc = LegacyConfig(-20.0f, 8.0f, 5.0f, 50.0f, 0.0f, /*sc*/42);
        cc.sidechainHpfHz = 0.0f;
        CompressorEffect c(cc);
        c.Prepare(kSampleRate, kChannels);
        std::vector<float> main = mainBuf;
        c.Process(main.data(), frames, kChannels, sineSide.data(), kChannels);
        EXPECT(c.CurrentReductionDb() > 3.0f);
    }

    // With HPF at 200 Hz: 60 Hz is well below cutoff, gets attenuated by
    // the filter, detector sees a quieter signal, less compression.
    {
        auto cc = LegacyConfig(-20.0f, 8.0f, 5.0f, 50.0f, 0.0f, /*sc*/42);
        cc.sidechainHpfHz = 200.0f;
        CompressorEffect c(cc);
        c.Prepare(kSampleRate, kChannels);
        std::vector<float> main = mainBuf;
        c.Process(main.data(), frames, kChannels, sineSide.data(), kChannels);
        EXPECT(c.CurrentReductionDb() < 1.0f);
    }
}

// Test 9. Hold delays release.
//   Loud sidechain pulse → reduction engages.
//   Sidechain drops to silence; with hold=100ms, reduction stays high
//   for at least the hold duration before release kicks in.
void TestHoldDelaysRelease() {
    auto cc = LegacyConfig(-20.0f, 8.0f, 1.0f, 30.0f, 0.0f, /*sc*/42);
    cc.holdMs = 100.0f;
    CompressorEffect c(cc);
    c.Prepare(kSampleRate, kChannels);

    // Pulse: loud sidechain for 50ms.
    const uint32_t pulseFrames = static_cast<uint32_t>(kSampleRate * 0.05);
    auto mainPulse = ConstantBuffer(pulseFrames, 0.1f);
    auto sidePulse = ConstantBuffer(pulseFrames, 1.0f);
    c.Process(mainPulse.data(), pulseFrames, kChannels,
                sidePulse.data(), kChannels);
    const float duringReduction = c.CurrentReductionDb();
    EXPECT(duringReduction > 5.0f);

    // 50ms of silent sidechain — within the 100ms hold window, so
    // reduction should still be substantial (release hasn't engaged yet).
    const uint32_t holdCheckFrames = static_cast<uint32_t>(kSampleRate * 0.05);
    auto mainHold = ConstantBuffer(holdCheckFrames, 0.1f);
    auto sideHold = ConstantBuffer(holdCheckFrames, 0.0f);
    c.Process(mainHold.data(), holdCheckFrames, kChannels,
                sideHold.data(), kChannels);
    const float duringHold = c.CurrentReductionDb();

    // Reduction should still be > half of peak — release hasn't kicked
    // in because we're inside the hold window.
    EXPECT(duringHold > duringReduction * 0.5f);

    // Now wait past the hold window + release — reduction should drop.
    const uint32_t restFrames = static_cast<uint32_t>(kSampleRate * 0.3);
    auto mainRest = ConstantBuffer(restFrames, 0.1f);
    auto sideRest = ConstantBuffer(restFrames, 0.0f);
    c.Process(mainRest.data(), restFrames, kChannels,
                sideRest.data(), kChannels);
    EXPECT(c.CurrentReductionDb() < 1.0f);
}

// Test 10. RMS vs Peak detection respond differently when the signal
// has amplitude variation that the envelope follower must average
// (rather than track). The impl applies an envelope follower to the
// detection signal — to |sample| in Peak mode, to sample² in RMS
// mode — so the modes produce *identical* dB readings whenever the
// follower fully tracks peaks (fast attack on constant-peak signal:
// 10·log10(peak²) == 20·log10(peak)).
//
// The discriminating case: a signal alternating between LOUD (1.0)
// and QUIET (0.1) at high frequency, with SLOW attack/release so
// the follower averages rather than tracks. Squaring weights loud
// samples disproportionately:
//
//   Peak average: (1.0 + 0.1) / 2 = 0.55     → 20·log10(0.55) ≈ -5.2 dB
//   RMS  average: (1.0² + 0.1²) / 2 = 0.505  → 10·log10(0.505) ≈ -3.0 dB
//
// → ~2 dB detection difference (RMS reads HIGHER, since the loud
//   half dominates a squared average more than a linear one).
// → with ratio 8:1 that's ~1.8 dB difference in reduction.
void TestRmsVsPeakDetection() {
    auto runWithMode = [](EffectConfig::CompressorDetectionMode mode) {
        // Use attack == release so the Peak follower is functionally
        // symmetric (no bias toward loud or quiet half). The
        // discriminating property under test is the squaring weight in
        // RMS — not the asymmetric attack/release of Peak. With
        // unequal attack/release, Peak's asymmetry would bias the
        // follower toward whichever direction is faster, which is a
        // separate axis of behavior.
        auto cc = LegacyConfig(-20.0f, 8.0f, 200.0f, 200.0f, 0.0f, kInvalidBusId);
        cc.detectionMode = mode;
        CompressorEffect c(cc);
        c.Prepare(kSampleRate, kChannels);

        // 500 ms of alternating loud/quiet samples at very high
        // rate (every other sample). That's well above any
        // realistic attack frequency, so the smoother averages.
        const uint32_t frames = static_cast<uint32_t>(kSampleRate * 0.5);
        std::vector<float> buf(frames);
        for (uint32_t i = 0; i < frames; ++i) {
            buf[i] = (i & 1) ? 1.0f : 0.1f;
        }
        c.Process(buf.data(), frames, kChannels, nullptr, 0);
        return c.CurrentReductionDb();
    };

    const float peakReduction =
        runWithMode(EffectConfig::CompressorDetectionMode::Peak);
    const float rmsReduction =
        runWithMode(EffectConfig::CompressorDetectionMode::Rms);

    EXPECT(peakReduction > 5.0f);
    EXPECT(rmsReduction  > 5.0f);

    // RMS reads ~2 dB above Peak for this signal because squaring
    // weights the loud half more heavily — so RMS produces MORE
    // reduction. Direction matters for the test, not the magnitude.
    EXPECT(rmsReduction > peakReduction + 1.0f);
}

} // namespace

int main() {
    // Pre-Tier-A regression coverage.
    TestNoSidechainQuietSignal();
    TestSelfSidechainLoudSignal();
    TestSidechainDucksQuietMain();
    TestSidechainReleaseRecovers();
    // Tier A coverage (v0.8.0).
    TestSoftKneeMeasurableTransition();
    TestMixRatioBlend();
    TestMaxReductionCap();
    TestSidechainHpfFilters();
    TestHoldDelaysRelease();
    TestRmsVsPeakDetection();
    std::printf(gFails == 0 ? "OK\n" : "FAILED (%d)\n", gFails);
    return gFails == 0 ? 0 : 1;
}
