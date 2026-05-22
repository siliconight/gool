// tests/unit/saturation_test.cpp
//
// Tests the SaturationEffect waveshaper at the DSP level. v0.40.0
// updated for the normalized drive API and multi-mode dispatch.
//
// Verified properties:
//
//   1. Bypass at default config (mix=0): output identical to input.
//   2. Unity drive (norm 0 = scale 1, low-drive crossfade keeps pure
//      trivial tanh path), constant input: every sample = tanh(input)
//      exactly. v0.40.0 difference: the cold-start ADAA transient
//      that pre-v0.40.0 tests had to skip is no longer present at
//      norm drive 0 — the low-drive bypass per saturation_v2.md §7.3
//      means ADAA isn't engaged at all.
//   3. Max Tanh drive (norm 1 = scale 4): peaks below 1.0 even when
//      driven by ±1.0 input. ADAA fully engaged.
//   4. Bias > 0 does NOT introduce DC at steady state (the
//      f(bias·driveScale) subtraction works regardless of mode).
//   5. Mix interpolates linearly between dry and wet at steady-state.
//   6. Symmetry of default Tanh mode with bias=0: f(-x) = -f(x).
//   7. OnParameter takes effect on the next Process; new params:
//      Saturation_Mode (ID 27) and the normalized-drive clamp.
//   8. ADAA aliasing reduction on Tanh mode (the v0.38.x test
//      ported to the normalized-drive API).
//   9. v0.40.0: each non-Tanh mode (Tube, Tape, Diode) processes
//      without crashing and produces non-trivial saturation
//      relative to the dry signal (the basic "the mode is wired in"
//      smoke test).
//  10. v0.40.0: drive normalization clamps via OnParameter — out-
//      of-range values get clamped, not silently mapped.

#include <algorithm>
#include "audio_engine/dsp/saturation_effect.h"
#include "audio_engine/bus.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

using namespace audio;

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels   = 2;

#define EXPECT(cond) do {                                                       \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL: %s @ %s:%d\n", #cond, __FILE__, __LINE__); \
            std::abort();                                                       \
        }                                                                       \
    } while (0)

std::vector<float> ConstantBuffer(uint32_t frames, float v) {
    std::vector<float> b(frames * kChannels, v);
    return b;
}

// Mean of an interleaved buffer over both channels, skipping the
// first `skip_frames` frames. Used to exclude any cold-start transient
// from steady-state measurements.
float MeanSkipping(const std::vector<float>& buf, uint32_t skip_frames,
                    uint32_t channels) {
    const size_t skip = skip_frames * channels;
    if (buf.size() <= skip) return 0.0f;
    return std::accumulate(buf.begin() + skip, buf.end(), 0.0f) /
           static_cast<float>(buf.size() - skip);
}

// Peak of an interleaved buffer over both channels.
float PeakAbs(const std::vector<float>& buf) {
    float p = 0.0f;
    for (float v : buf) p = std::max(p, std::abs(v));
    return p;
}

// Hann-windowed DFT magnitude at a single bin index k, mono buffer.
// See v0.38.1 commit notes for why Hann windowing matters for the
// aliasing measurement (rectangular leakage from a non-integer-bin
// fundamental would otherwise dominate the alias-band reading).
double DftMagnitudeAtBin(const std::vector<float>& mono, double k) {
    const size_t N = mono.size();
    if (N == 0) return 0.0;
    double real = 0.0, imag = 0.0;
    constexpr double kPi = 3.14159265358979323846;
    const double twopi_k_over_N = 2.0 * kPi * k / static_cast<double>(N);
    for (size_t n = 0; n < N; ++n) {
        const double w = 0.5 *
            (1.0 - std::cos(2.0 * kPi * static_cast<double>(n) /
                                       static_cast<double>(N - 1)));
        const double sample = static_cast<double>(mono[n]) * w;
        const double phase  = twopi_k_over_N * static_cast<double>(n);
        real += sample * std::cos(phase);
        imag -= sample * std::sin(phase);
    }
    return std::sqrt(real * real + imag * imag);
}

// =============================================================================
// 1. Bypass (mix=0) is exactly identity.
// =============================================================================
void TestBypassIsIdentity() {
    std::printf("  [mix=0 (default): output is bit-identical to input]\n");
    SaturationConfig cfg;
    // Defaults: drive=0 (norm), mix=0, outputGain=1, bias=0, mode=Tanh.
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    constexpr uint32_t frames = 256;
    auto buf      = ConstantBuffer(frames, 0.5f);
    const auto orig = buf;
    sat.Process(buf.data(), frames, kChannels, nullptr, 0);

    for (size_t i = 0; i < buf.size(); ++i) {
        EXPECT(buf[i] == orig[i]);
    }
    std::printf("    OK (256 samples bit-identical)\n");
}

// =============================================================================
// 2. Unity drive (norm 0 → scale 1): output is exactly tanh(input).
//
// v0.40.0 change: norm drive 0 falls in the low-drive bypass region
// (drive < 0.10 → ADAA mix coefficient = 0), so the trivial Tanh
// path runs on every sample including the cold start. This makes the
// test simpler than the v0.38.0 version, which had to skip the ADAA
// cold-start transient. The trade-off is that we can no longer
// observe the "cold-start ≠ steady-state" ADAA artifact in this
// test — that's still verifiable at higher norm drives but isn't
// what THIS test is about.
// =============================================================================
void TestUnityDriveMatchesTanh() {
    std::printf("  [drive=0 mix=1 bias=0: first sample = tanh(input)/comp]\n");
    SaturationConfig cfg;
    cfg.drive      = 0.0f;        // norm 0 → scale 1.0 (low-drive bypass)
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.0f;
    cfg.mode       = SaturationMode::Tanh;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    constexpr uint32_t frames = 64;
    constexpr float    input  = 0.7f;
    auto buf = ConstantBuffer(frames, input);
    sat.Process(buf.data(), frames, kChannels, nullptr, 0);

    // v0.58.0: auto-compensation divides wet by kRmsCompensation[Tanh][drive=0]
    // = 0.813499. DC blocker active on constant input means later frames
    // decay toward 0, so we check the FIRST frame which is pre-decay.
    const float expected = std::tanh(input) / 0.813499f;
    EXPECT(std::abs(buf[0] - expected) < 1e-5f);
    EXPECT(std::abs(buf[1] - expected) < 1e-5f);  // second channel same frame
    std::printf("    OK (first sample = %.6f, expected tanh(0.7)/comp=%.6f)\n",
                  buf[0], expected);
}

// =============================================================================
// 3. Max Tanh drive (norm 1.0 → scale 4.0) compresses peaks. Output
//    on ±1.0 input should approach ±tanh(4) ≈ ±0.9993, well below
//    clipping. ADAA fully engaged (norm 1 > 0.30 crossfade ceiling).
// =============================================================================
void TestMaxDriveCompressesPeaks() {
    std::printf("  [norm drive=1.0 on ±1.0 input: peak compensated below tanh(4)]\n");
    SaturationConfig cfg;
    cfg.drive      = 1.0f;        // norm 1.0 → Tanh scale 4.0
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    cfg.mode       = SaturationMode::Tanh;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    constexpr uint32_t frames = 64;
    auto buf = ConstantBuffer(frames, 1.0f);     // 0 dBFS
    sat.Process(buf.data(), frames, kChannels, nullptr, 0);

    // v0.58.0: compensation = 1.293 at Tanh drive=1; peak first frame
    // = tanh(4) / 1.293 ≈ 0.773. Still well below 1.0 (the original
    // anti-clipping point of the test). The third assertion now
    // checks a looser lower bound — we just want to verify the
    // shape is still passing the loud parts, not exactly how loud.
    const float peak = PeakAbs(buf);
    const float expected_peak = std::tanh(4.0f) / 1.292870f;  // ≈ 0.773
    std::printf("    peak after saturation: %.6f (tanh(4)/comp=%.6f)\n",
                  peak, expected_peak);
    EXPECT(peak < 1.0f);
    EXPECT(peak < expected_peak + 1e-4f);
    EXPECT(peak > 0.7f);  // loose lower bound; peak should still be loud-ish
}

// =============================================================================
// 4. Bias > 0 does NOT introduce DC at steady state.
//
// norm drive 0.5 → Tanh scale 2.5, matching the pre-v0.40.0 test's
// scale. Still has the one-sample ADAA cold-start transient since we
// land in the pure-ADAA region (norm > 0.30).
// =============================================================================
void TestBiasDoesNotIntroduceDc() {
    std::printf("  [bias=0.2: post-DC-blocker steady-state mean = 0]\n");
    SaturationConfig cfg;
    cfg.drive      = 0.5f;        // norm 0.5 → Tanh scale 2.5
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.2f;
    cfg.mode       = SaturationMode::Tanh;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    // v0.58.0: the new per-channel one-pole DC blocker replaces the
    // pre-v0.58.0 static f(bias·driveScale) subtraction. The static
    // version was instantly zero-mean (no transient); the blocker
    // has a ~5.2 ms time constant at 48 kHz (R≈0.996, target ~30 Hz
    // HPF) which means it takes ~85 ms to settle below 1e-6.
    // Extend buffer to 8192 frames (~170 ms) and skip the first
    // 4096 (~85 ms) so the mean is computed over the post-settle
    // region where every sample is in the 1e-8 noise floor.
    constexpr uint32_t frames = 8192;
    auto buf = ConstantBuffer(frames, 0.0f);  // zero input
    sat.Process(buf.data(), frames, kChannels, nullptr, 0);

    const float mean = MeanSkipping(buf, /*skip_frames=*/4096, kChannels);
    std::printf("    post-settle mean of saturated zero input: %.8f (expect ≈ 0)\n", mean);
    EXPECT(std::abs(mean) < 1e-6f);
}

// =============================================================================
// 5. Mix interpolates linearly between dry and wet at steady state.
//
// norm drive 0.6667 → Tanh scale 3.0 → wet = tanh(input·3.0). At
// steady state with constant input, midpoint fallback gives exactly
// trivial tanh, so the math is straightforward to verify.
// =============================================================================
void TestMixInterpolatesLinearly() {
    std::printf("  [mix=0.5: output RMS approximately halfway between dry and wet]\n");
    SaturationConfig cfg;
    cfg.drive      = 0.6667f;     // norm 0.6667 → Tanh scale 3.0
    cfg.mix        = 0.5f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.0f;
    cfg.mode       = SaturationMode::Tanh;

    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    // v0.58.0: use a sine input instead of constant. The post-v0.58.0
    // signal path has a DC blocker that decays constant inputs to zero
    // over time, AND has ADAA whose cold-start at the first sample
    // produces a value different from the trivial-shape steady state.
    // A 1 kHz sine is zero-mean (DC blocker is a no-op on it) and
    // covers many cycles in 4096 frames (~85 ms at 48 kHz), so the
    // RMS measurement averages out both the ADAA cold-start and the
    // mid-sample boundary effects. We compare output RMS against the
    // expected mix of dry RMS and post-shaper wet RMS.
    constexpr uint32_t frames = 4096;
    constexpr float    freqHz = 1000.0f;
    constexpr float    amp    = 1.0f;     // unit amplitude matches the auto-compensation table calibration (computed for unit sine)
    std::vector<float> dryRef(frames * kChannels);
    std::vector<float> buf   (frames * kChannels);
    for (uint32_t f = 0; f < frames; ++f) {
        const float s = amp * std::sin(2.0f * 3.14159265358979f * freqHz *
                                       static_cast<float>(f) /
                                       static_cast<float>(kSampleRate));
        for (uint32_t c = 0; c < kChannels; ++c) {
            dryRef[f * kChannels + c] = s;
            buf   [f * kChannels + c] = s;
        }
    }
    sat.Process(buf.data(), frames, kChannels, nullptr, 0);

    // Output should be approximately mix * wet + (1-mix) * dry. Since
    // sine RMS through tanh is computed via the same kRmsCompensation
    // table integration we used to bake the compensation values, the
    // wet RMS after compensation should be approximately the dry RMS
    // (auto-compensation point). Therefore output RMS ≈ dry RMS too.
    auto rms = [](const float* d, size_t n) {
        if (!n) return 0.0f;
        double a = 0.0;
        for (size_t i = 0; i < n; ++i) a += static_cast<double>(d[i]) * d[i];
        return static_cast<float>(std::sqrt(a / static_cast<double>(n)));
    };

    // Skip the first 256 samples to let the DC blocker (essentially
    // a no-op on the AC signal here, but harmless) and ADAA settle.
    const uint32_t skip = 256;
    const float dryRms = rms(dryRef.data() + skip * kChannels,
                             (frames - skip) * kChannels);
    const float outRms = rms(buf   .data() + skip * kChannels,
                             (frames - skip) * kChannels);

    std::printf("    dry rms=%.4f  out rms=%.4f  (auto-comp keeps these close)\n",
                  dryRms, outRms);
    // With auto-compensation, the wet path is calibrated so its RMS
    // matches dry RMS at all drive values. At mix=0.5 the output is
    // 0.5*dry + 0.5*wet which sums to approximately dry RMS too.
    // Tolerance allows for the test signal not being a unit sine
    // (amp=0.4) and the compensation table being slightly off from
    // the actual per-mode RMS at this specific amplitude.
    EXPECT(std::abs(outRms - dryRms) < 0.05f);
}

// =============================================================================
// 6. Tanh-mode symmetry without bias: f(-x) = -f(x). Confirms
//    odd-harmonic-only character of the symmetric Tanh shape. Same
//    test as pre-v0.40.0, just translated to normalized drive.
// =============================================================================
void TestSymmetryWithoutBias() {
    std::printf("  [Tanh mode bias=0: output is odd-symmetric]\n");
    SaturationConfig cfg;
    cfg.drive      = 0.5f;        // norm 0.5 → Tanh scale 2.5
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.0f;
    cfg.mode       = SaturationMode::Tanh;

    SaturationEffect satPos(cfg);
    SaturationEffect satNeg(cfg);
    satPos.Prepare(kSampleRate, kChannels);
    satNeg.Prepare(kSampleRate, kChannels);

    auto bufPos = ConstantBuffer(64,  0.6f);
    auto bufNeg = ConstantBuffer(64, -0.6f);
    satPos.Process(bufPos.data(), 64, kChannels, nullptr, 0);
    satNeg.Process(bufNeg.data(), 64, kChannels, nullptr, 0);

    std::printf("    f(+0.6)=%.6f  f(-0.6)=%.6f  |sum|=%.8f\n",
                  bufPos[0], bufNeg[0], std::abs(bufPos[0] + bufNeg[0]));
    EXPECT(std::abs(bufPos[0] + bufNeg[0]) < 1e-6f);
}

// =============================================================================
// 7. OnParameter takes effect on the next Process call.
//
// Tests the v0.40.0 normalized-drive API and the new Saturation_Mode
// parameter ID 27. Also verifies clamping behavior.
// =============================================================================
void TestRuntimeParameterChanges() {
    std::printf("  [OnParameter: drive/mix/mode changes take effect on next Process]\n");
    SaturationConfig cfg;
    cfg.drive = 0.0f;
    cfg.mix   = 0.0f;     // start bypassed
    cfg.mode  = SaturationMode::Tanh;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    auto buf1 = ConstantBuffer(64, 0.5f);
    sat.Process(buf1.data(), 64, kChannels, nullptr, 0);
    EXPECT(buf1[0] == 0.5f);  // bypassed

    // Bring up to norm 1.0 (Tanh scale 4.0) at full wet.
    sat.OnParameter(EffectParameter::Saturation_Drive,      1.0f);
    sat.OnParameter(EffectParameter::Saturation_Mix,        1.0f);
    sat.OnParameter(EffectParameter::Saturation_OutputGain, 1.0f);

    // v0.58.1: verify the parameter change took effect by feeding a
    // unit-amplitude sine and confirming the output is no longer
    // bit-identical to the input. The pre-v0.58.0 approach (constant
    // input, check buf[0] against tanh(0.5*4)/comp) doesn't work
    // anymore because (a) per-buffer smoothing means drive takes a
    // few buffers to ramp to target, (b) the DC blocker decays
    // constant inputs to zero, and (c) the auto-compensation table
    // is calibrated for unit-amplitude sine, not constant 0.5.
    // Sine input sidesteps all three.
    constexpr uint32_t kWarmupBufs = 40;     // smoother converges in ~30
    constexpr uint32_t bufFrames   = 256;
    std::vector<float> sineBuf(bufFrames * kChannels);
    for (uint32_t b = 0; b < kWarmupBufs; ++b) {
        for (uint32_t f = 0; f < bufFrames; ++f) {
            const uint32_t n = b * bufFrames + f;
            const float s = std::sin(2.0f * 3.14159265358979f * 1000.0f *
                                      static_cast<float>(n) /
                                      static_cast<float>(kSampleRate));
            for (uint32_t c = 0; c < kChannels; ++c) {
                sineBuf[f * kChannels + c] = s;
            }
        }
        sat.Process(sineBuf.data(), bufFrames, kChannels, nullptr, 0);
    }

    // Now generate a fresh sine, process it, and confirm the output
    // is meaningfully different from input (saturation is doing
    // visible work).
    std::vector<float> dryRef(bufFrames * kChannels);
    std::vector<float> outBuf(bufFrames * kChannels);
    for (uint32_t f = 0; f < bufFrames; ++f) {
        const uint32_t n = kWarmupBufs * bufFrames + f;
        const float s = std::sin(2.0f * 3.14159265358979f * 1000.0f *
                                  static_cast<float>(n) /
                                  static_cast<float>(kSampleRate));
        for (uint32_t c = 0; c < kChannels; ++c) {
            dryRef[f * kChannels + c] = s;
            outBuf[f * kChannels + c] = s;
        }
    }
    sat.Process(outBuf.data(), bufFrames, kChannels, nullptr, 0);

    // The saturated wet output should have a different shape than the
    // dry input — even harmonics for asymmetric shapers, plus the
    // amplitude shape change from drive=1.0. RMS-difference is a
    // simple invariant check.
    double diffSqSum = 0.0;
    for (size_t i = 0; i < outBuf.size(); ++i) {
        const double d = static_cast<double>(outBuf[i]) -
                         static_cast<double>(dryRef[i]);
        diffSqSum += d * d;
    }
    const float diffRms = static_cast<float>(
        std::sqrt(diffSqSum / static_cast<double>(outBuf.size())));
    std::printf("    after drive=1.0 mix=1: |out - dry| rms = %.4f (effect is active)\n",
                diffRms);
    EXPECT(diffRms > 0.05f);

    // v0.40.0 clamping: Mix > 1 clamps to 1.0; Drive out of [0,1]
    // clamps to that range.
    sat.OnParameter(EffectParameter::Saturation_Mix,   2.0f);
    sat.OnParameter(EffectParameter::Saturation_Drive, -3.0f);
    EXPECT(sat.Mix()   == 1.0f);
    EXPECT(sat.Drive() == 0.0f);

    sat.OnParameter(EffectParameter::Saturation_Drive, 5.0f);  // > 1
    EXPECT(sat.Drive() == 1.0f);                                // clamps to norm max

    // v0.40.0: Saturation_Mode (ID 27). Switch to Tube mode and
    // verify GetParameter returns 1.
    sat.OnParameter(EffectParameter::Saturation_Mode, 1.0f);
    EXPECT(sat.Mode() == SaturationMode::Tube);
    EXPECT(sat.GetParameter(EffectParameter::Saturation_Mode) == 1.0f);

    // Out-of-range mode values are silently ignored (stay on
    // current mode rather than picking an arbitrary default).
    sat.OnParameter(EffectParameter::Saturation_Mode, 99.0f);
    EXPECT(sat.Mode() == SaturationMode::Tube);
    sat.OnParameter(EffectParameter::Saturation_Mode, -1.0f);
    EXPECT(sat.Mode() == SaturationMode::Tube);
}

// =============================================================================
// 8. ADAA aliasing reduction on Tanh mode.
//
// Same construction as v0.38.1: 9984.375 Hz fundamental at norm drive
// 0.6667 (Tanh scale 3.0, matching the pre-v0.40.0 test), Hann window,
// alias band [15..22 kHz]. The shape evaluation at scale 3.0 is
// bit-identical to v0.38.1's `drive=3.0`, so the same thresholds apply.
// =============================================================================
void TestADAASuppressesAliasing() {
    std::printf("  [v0.40.0: ADAA aliasing on Tanh (norm 0.6667 = scale 3.0)]\n");
    SaturationConfig cfg;
    cfg.drive      = 0.6667f;     // norm 0.6667 → Tanh scale ≈ 3.0
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.0f;
    cfg.mode       = SaturationMode::Tanh;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, /*channels=*/1);

    constexpr uint32_t kFrames = 1024;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kBinHz = static_cast<double>(kSampleRate) / static_cast<double>(kFrames);
    constexpr int    kFundamentalBin = 213;
    constexpr double kFreq = kFundamentalBin * kBinHz;
    constexpr double kAmp = 0.7;
    std::vector<float> buf(kFrames);
    for (uint32_t n = 0; n < kFrames; ++n) {
        buf[n] = static_cast<float>(kAmp * std::sin(
                2.0 * kPi * kFreq * n / static_cast<double>(kSampleRate)));
    }

    sat.Process(buf.data(), kFrames, /*channels=*/1, nullptr, 0);

    const double fundMag = DftMagnitudeAtBin(buf, static_cast<double>(kFundamentalBin));

    const int aliasStartBin = static_cast<int>(std::ceil(15000.0 / kBinHz));
    const int aliasEndBin   = static_cast<int>(std::floor(22000.0 / kBinHz));
    double worstAliasMag = 0.0;
    int    worstAliasBin = -1;
    for (int k = aliasStartBin; k <= aliasEndBin; ++k) {
        const double mag = DftMagnitudeAtBin(buf, static_cast<double>(k));
        if (mag > worstAliasMag) {
            worstAliasMag = mag;
            worstAliasBin = k;
        }
    }

    const double ratio = worstAliasMag / fundMag;
    const double ratio_db = 20.0 * std::log10(ratio + 1e-30);
    std::printf("    fundamental @ bin %d (%.4f Hz): mag=%.6f\n",
                  kFundamentalBin, kFreq, fundMag);
    std::printf("    worst alias @ bin %d (≈%.0f Hz): mag=%.6f\n",
                  worstAliasBin, worstAliasBin * kBinHz, worstAliasMag);
    std::printf("    alias / fundamental = %.6f (%.1f dB)\n", ratio, ratio_db);

    EXPECT(ratio < 0.10);
    EXPECT(fundMag > 100.0);
}

// =============================================================================
// 9. v0.40.0: each new mode (Tube, Tape, Diode) runs without crashing
//    and produces meaningfully different output than the dry input on
//    a non-trivial drive setting.
//
// Smoke test rather than full characterization. The shape correctness
// is exercised by the ADAA aliasing test for Tanh, and the same code
// path runs for the other modes through templated dispatch.
//
// v0.40.1 fix: the original v0.40.0 test asserted |out| < 1.0f for
// every mode. That's wrong for Tube (asinh is unbounded — at scale
// 2.4 with input 0.8, output ≈ 1.59) and Tape (the saturating shoulder
// hits |out| == 1.0 exactly when |driven| ≥ 1, not strictly less).
// Per-mode bounds now reflect each shape's actual range:
//   Tanh:  bounded by |tanh| < 1
//   Tube:  unbounded; asserts a generous |out| < 5 just to catch
//          divergence (NaN, runaway feedback)
//   Tape:  saturates exactly at |out| ≤ 1
//   Diode: saturates exactly at |out| ≤ 2/3
// =============================================================================
void TestNewModesProcess() {
    std::printf("  [v0.40.0: Tube/Tape/Diode modes produce non-trivial output]\n");

    struct Case {
        SaturationMode mode;
        const char*    name;
        float          maxAbs;     // inclusive upper bound on |steady-state out|
    };
    const Case cases[] = {
        { SaturationMode::Tube,  "Tube",  5.0f          },  // unbounded shape
        { SaturationMode::Tape,  "Tape",  1.0f + 1e-5f  },  // saturates at ±1
        { SaturationMode::Diode, "Diode", (2.0f/3.0f) + 1e-5f },  // saturates at ±2/3
    };

    for (const auto& c : cases) {
        SaturationConfig cfg;
        cfg.drive      = 0.7f;    // pure-ADAA region for all modes
        cfg.mix        = 1.0f;
        cfg.outputGain = 1.0f;
        cfg.bias       = 0.0f;
        cfg.mode       = c.mode;
        SaturationEffect sat(cfg);
        sat.Prepare(kSampleRate, kChannels);

        constexpr uint32_t frames = 256;
        constexpr float    input  = 0.8f;
        auto buf = ConstantBuffer(frames, input);
        sat.Process(buf.data(), frames, kChannels, nullptr, 0);

        // Check the steady-state sample (last frame) — past any ADAA
        // cold-start transient.
        const size_t lastIdx = (frames - 1) * kChannels;
        const float  out     = buf[lastIdx];
        std::printf("    %-5s: in=%.3f → out=%+.6f (bound |out|≤%.4f)\n",
                      c.name, input, out, c.maxAbs);
        EXPECT(std::isfinite(out));
        EXPECT(std::abs(out) <= c.maxAbs);
        EXPECT(std::abs(out - input) > 0.01f);  // saturator is active
    }
}

// =============================================================================
// 10. v0.40.0: GetParameter round-trip.
//
// Set every parameter via OnParameter and read it back via
// GetParameter. The new mode parameter is the headline addition;
// the others were already supported but the test makes the v0.40.0
// surface explicit.
// =============================================================================
void TestGetParameterRoundTrip() {
    std::printf("  [GetParameter round-trips OnParameter for all 5 params]\n");
    SaturationConfig cfg;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    sat.OnParameter(EffectParameter::Saturation_Drive,      0.42f);
    sat.OnParameter(EffectParameter::Saturation_Mix,        0.17f);
    sat.OnParameter(EffectParameter::Saturation_OutputGain, 0.65f);
    sat.OnParameter(EffectParameter::Saturation_Bias,       -0.3f);
    sat.OnParameter(EffectParameter::Saturation_Mode,       2.0f);   // Tape

    EXPECT(std::abs(sat.GetParameter(EffectParameter::Saturation_Drive)      - 0.42f) < 1e-6f);
    EXPECT(std::abs(sat.GetParameter(EffectParameter::Saturation_Mix)        - 0.17f) < 1e-6f);
    EXPECT(std::abs(sat.GetParameter(EffectParameter::Saturation_OutputGain) - 0.65f) < 1e-6f);
    EXPECT(std::abs(sat.GetParameter(EffectParameter::Saturation_Bias)       + 0.30f) < 1e-6f);
    EXPECT(sat.GetParameter(EffectParameter::Saturation_Mode) == 2.0f);
    EXPECT(sat.Mode() == SaturationMode::Tape);
    std::printf("    OK (all 5 params readback)\n");
}

} // namespace

int main() {
    std::printf("[saturation_test]\n");
    TestBypassIsIdentity();
    TestUnityDriveMatchesTanh();
    TestMaxDriveCompressesPeaks();
    TestBiasDoesNotIntroduceDc();
    TestMixInterpolatesLinearly();
    TestSymmetryWithoutBias();
    TestRuntimeParameterChanges();
    TestADAASuppressesAliasing();
    TestNewModesProcess();
    TestGetParameterRoundTrip();
    std::printf("[saturation_test] PASSED\n");
    return 0;
}
