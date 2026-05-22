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
    std::printf("  [bias=0.2: steady-state DC mean = 0 (scale 2.5, ADAA active)]\n");
    SaturationConfig cfg;
    cfg.drive      = 0.5f;        // norm 0.5 → Tanh scale 2.5
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.2f;
    cfg.mode       = SaturationMode::Tanh;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    constexpr uint32_t frames = 1024;
    auto buf = ConstantBuffer(frames, 0.0f);  // zero input
    sat.Process(buf.data(), frames, kChannels, nullptr, 0);

    const float mean = MeanSkipping(buf, /*skip_frames=*/4, kChannels);
    std::printf("    steady-state mean of saturated zero input: %.8f (expect ≈ 0)\n", mean);
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
    std::printf("  [mix=0.5: first sample is halfway between dry and compensated wet]\n");
    SaturationConfig cfg;
    cfg.drive      = 0.6667f;     // norm 0.6667 → Tanh scale 3.0
    cfg.mix        = 0.5f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.0f;
    cfg.mode       = SaturationMode::Tanh;

    constexpr float input = 0.4f;

    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);
    auto buf = ConstantBuffer(64, input);
    sat.Process(buf.data(), 64, kChannels, nullptr, 0);

    // v0.58.0: wet = tanh(input·3.0) / comp(Tanh, 0.6667).
    // Compensation lookup interpolates between breakpoints 0.5
    // (1.2025) and 0.75 (1.2601): at 0.6667 → t=0.6668 → ≈ 1.2409.
    // First-sample assertion avoids the DC blocker decay on the
    // (artificial) constant input.
    constexpr float comp_0667 = 1.2409f;
    const float wet      = std::tanh(input * 3.0f) / comp_0667;
    const float expected = 0.5f * input + 0.5f * wet;
    const float observed = buf[0];
    std::printf("    dry=%.4f wet=%.4f mix=0.5 → expected=%.4f got=%.4f (frame 0)\n",
                  input, wet, expected, observed);
    EXPECT(std::abs(observed - expected) < 1e-3f);
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

    // Bring up to norm 1.0 (Tanh scale 4.0) at full wet, then check
    // the post-smoother output matches tanh(0.5 * 4.0) / compensation.
    sat.OnParameter(EffectParameter::Saturation_Drive,      1.0f);
    sat.OnParameter(EffectParameter::Saturation_Mix,        1.0f);
    sat.OnParameter(EffectParameter::Saturation_OutputGain, 1.0f);

    // v0.58.0: drive/mix/bias are per-buffer-smoothed (coefficient
    // 0.25). After ~30 buffers the smoother converges to within ~1%.
    // Run enough warmup buffers to let it settle before asserting.
    auto warmup = ConstantBuffer(64, 0.5f);
    for (int i = 0; i < 40; ++i) {
        sat.Process(warmup.data(), 64, kChannels, nullptr, 0);
        // Re-fill so each buffer sees the same constant input.
        std::fill(warmup.begin(), warmup.end(), 0.5f);
    }

    auto buf2 = ConstantBuffer(64, 0.5f);
    sat.Process(buf2.data(), 64, kChannels, nullptr, 0);
    // v0.58.0: expected = tanh(0.5 * 4) / kRmsCompensation[Tanh][1.0].
    // First frame of buf2 is before this buffer's DC blocker decay
    // accumulates (the blocker state from warmup is also near zero
    // since constant input through blocker → output → 0 over time).
    const float expected = std::tanh(0.5f * 4.0f) / 1.292870f;
    const float observed = buf2[0];
    std::printf("    after drive=1.0 (Tanh scale 4) mix=1: frame0=%.6f expected=%.6f\n",
                  observed, expected);
    // Loose tolerance — smoother is at ~99% converged after 40
    // buffers, plus the DC blocker may have residual settling.
    EXPECT(std::abs(observed - expected) < 0.05f);

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
