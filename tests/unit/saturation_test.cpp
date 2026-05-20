// tests/unit/saturation_test.cpp
//
// Tests the SaturationEffect waveshaper at the DSP level. Verifies:
//
//   1. Bypass at default config (mix=0): output identical to input.
//   2. Drive=1, mix=1, bias=0, constant input: steady-state output
//      = tanh(input) * outputGain. (v0.38.0: ADAA's first sample is
//      the integral-average between cold-start state x[n-1]=0 and
//      x[n]; from sample 2 onward the midpoint fallback fires on
//      constant input and gives exactly trivial tanh. Test checks
//      the steady-state value, not the cold-start transient.)
//   3. Drive>1 produces compression: peaks below 1.0 even when
//      driven above 1.0.
//   4. DC bias does NOT introduce DC at the output (excluding the
//      one-sample ADAA cold-start transient): the tanh(bias*drive)
//      subtraction works on steady-state.
//   5. Mix interpolates linearly between dry and wet at steady-state.
//   6. Symmetry of default tanh: output(-x) == -output(x) when
//      bias=0. Confirms odd-harmonic-only character of the
//      symmetric mode. (Also holds at the cold-start sample because
//      log(cosh) is even, so ADAA inherits the symmetry.)
//   7. Runtime parameter updates take effect on the next Process.
//   8. v0.38.0: ADAA aliasing reduction. Feed a 19 kHz sine at
//      drive=3.0 through the shaper and assert that energy in the
//      [20 kHz .. 23 kHz] band is far below the fundamental. The
//      9th harmonic of 19 kHz folds back to 21 kHz at 48 kHz SR
//      under non-bandlimited tanh; ADAA suppresses this.

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
// first `skip_frames` frames. Used to exclude the ADAA cold-start
// transient from steady-state measurements.
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

// Brute-force DFT magnitude at a single bin, mono (assumes channels=1).
// Goertzel would be marginally faster but the readability win is more
// valuable than the cycles in a unit test. N samples, freq normalized
// as bin index k (i.e. evaluates the DFT at omega = 2*pi*k/N).
double DftMagnitudeAtBin(const std::vector<float>& mono, double k) {
    const size_t N = mono.size();
    if (N == 0) return 0.0;
    double real = 0.0, imag = 0.0;
    const double twopi_k_over_N = 2.0 * 3.14159265358979323846 * k / static_cast<double>(N);
    for (size_t n = 0; n < N; ++n) {
        const double phase = twopi_k_over_N * static_cast<double>(n);
        real += mono[n] * std::cos(phase);
        imag -= mono[n] * std::sin(phase);
    }
    return std::sqrt(real * real + imag * imag);
}

// =============================================================================
// 1. Bypass (mix=0) is exactly identity.
// =============================================================================
void TestBypassIsIdentity() {
    std::printf("  [bypass: mix=0 leaves signal untouched]\n");
    SaturationConfig cfg;
    // Defaults: drive=1, mix=0, outputGain=1, bias=0.
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
// 2. drive=1, mix=1, bias=0, constant input: STEADY-STATE output is tanh(input).
//
// v0.38.0 change: ADAA's first sample uses x[n-1]=0 (cold-start state) and
// computes the integral-mean of tanh over [0, input*drive], which differs
// from tanh(input*drive) at the cold-start sample. From sample 2 onward the
// midpoint fallback fires (since adjacent samples are equal on constant
// input) and the output is exactly trivial tanh. We test sample 32 to skip
// the cold-start transient.
// =============================================================================
void TestUnityDriveMatchesTanh() {
    std::printf("  [drive=1 mix=1 bias=0: steady-state output is tanh(input)]\n");
    SaturationConfig cfg;
    cfg.drive      = 1.0f;
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.0f;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    constexpr uint32_t frames = 64;
    constexpr float    input  = 0.7f;
    auto buf = ConstantBuffer(frames, input);
    sat.Process(buf.data(), frames, kChannels, nullptr, 0);

    const float expected = std::tanh(input);

    // Sample 0 is the cold-start transient — explicitly check it's NOT
    // equal to plain tanh (this is the ADAA "feature" the previous
    // test didn't tolerate, now codified).
    EXPECT(std::abs(buf[0] - expected) > 1e-3f);

    // From frame 1 onward, output should match trivial tanh to float
    // precision (the midpoint fallback gives identical math).
    constexpr uint32_t kSteadyStateFrame = 32;
    for (uint32_t f = kSteadyStateFrame; f < frames; ++f) {
        for (uint32_t c = 0; c < kChannels; ++c) {
            const size_t idx = f * kChannels + c;
            EXPECT(std::abs(buf[idx] - expected) < 1e-6f);
        }
    }
    std::printf("    OK (cold-start=%.6f, steady-state=%.6f, expected tanh=%.6f)\n",
                  buf[0], buf[kSteadyStateFrame * kChannels], expected);
}

// =============================================================================
// 3. Drive > 1 compresses peaks: tanh saturates above ~|3| toward ±1.
//
// Peak measurement is robust to ADAA cold-start since the cold-start
// transient is SMALLER than steady-state (integral-mean ≤ saturating
// endpoint), so the peak still reflects the steady-state saturated value.
// =============================================================================
void TestDriveCompressesPeaks() {
    std::printf("  [drive=4 on 1.0 input: output bounded by ~|tanh(4)|]\n");
    SaturationConfig cfg;
    cfg.drive      = 4.0f;
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    constexpr uint32_t frames = 64;
    auto buf = ConstantBuffer(frames, 1.0f);  // 0 dBFS
    sat.Process(buf.data(), frames, kChannels, nullptr, 0);

    // tanh(4) ≈ 0.9993. Output must be inside that envelope and below 1.0.
    const float peak = PeakAbs(buf);
    std::printf("    peak after saturation: %.6f (tanh(4)=%.6f)\n",
                  peak, std::tanh(4.0f));
    EXPECT(peak < 1.0f);
    EXPECT(peak < std::tanh(4.0f) + 1e-4f);
    EXPECT(peak > 0.99f);  // strongly saturated, not silent
}

// =============================================================================
// 4. Bias > 0 does NOT introduce DC at the output (tanh(bias*drive)
//    is subtracted internally), measured at steady-state.
//
// v0.38.0 change: the cold-start sample contributes a small one-time
// transient since ADAA at the boundary doesn't match the steady-state
// tanh(bias*drive) DC removal. Skip the first 4 frames to measure
// only the steady-state mean.
// =============================================================================
void TestBiasDoesNotIntroduceDc() {
    std::printf("  [bias=0.2: DC offset removed; steady-state mean = 0]\n");
    SaturationConfig cfg;
    cfg.drive      = 2.5f;
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.2f;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    constexpr uint32_t frames = 1024;
    auto buf = ConstantBuffer(frames, 0.0f);  // zero input
    sat.Process(buf.data(), frames, kChannels, nullptr, 0);

    // Skip the cold-start frame; measure mean over the remaining
    // 1020 frames × 2 channels.
    const float mean = MeanSkipping(buf, /*skip_frames=*/4, kChannels);
    std::printf("    steady-state mean of saturated zero input: %.8f (expect ≈ 0)\n", mean);
    EXPECT(std::abs(mean) < 1e-6f);  // DC removal works at steady-state
}

// =============================================================================
// 5. Mix interpolates linearly between dry and wet at steady-state.
//    At mix=0.5, steady-state output = 0.5*dry + 0.5*wet.
//
// v0.38.0 change: check sample 32 instead of sample 0 to skip the
// ADAA cold-start transient.
// =============================================================================
void TestMixInterpolatesLinearly() {
    std::printf("  [mix=0.5: steady-state output is halfway between dry and wet]\n");
    SaturationConfig cfg;
    cfg.drive      = 3.0f;
    cfg.mix        = 0.5f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.0f;

    constexpr float input = 0.4f;

    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);
    auto buf = ConstantBuffer(64, input);
    sat.Process(buf.data(), 64, kChannels, nullptr, 0);

    const float wet = std::tanh(input * 3.0f);
    const float expected = 0.5f * input + 0.5f * wet;
    constexpr uint32_t kSteadyStateFrame = 32;
    const float observed = buf[kSteadyStateFrame * kChannels];
    std::printf("    dry=%.4f wet=%.4f mix=0.5 → expected=%.4f got=%.4f (frame %u)\n",
                  input, wet, expected, observed, kSteadyStateFrame);
    EXPECT(std::abs(observed - expected) < 1e-6f);
}

// =============================================================================
// 6. Symmetry of default tanh: output(-x) == -output(x) when bias=0.
//    Confirms odd-harmonic-only character of the symmetric mode.
//
// v0.38.0 note: This holds even on the cold-start sample because
// log(cosh) is an even function, so the ADAA integral-mean inherits
// the sign symmetry from the underlying tanh. So this test is
// unchanged from v0.37.0 and still uses buf[0].
// =============================================================================
void TestSymmetryWithoutBias() {
    std::printf("  [bias=0: output is odd-symmetric (no even harmonics)]\n");
    SaturationConfig cfg;
    cfg.drive      = 2.5f;
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.0f;

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
// v0.38.0 change: check steady-state (sample 32) instead of sample 0
// after the parameter change.
// =============================================================================
void TestRuntimeParameterChanges() {
    std::printf("  [OnParameter: changes take effect on the next Process]\n");
    SaturationConfig cfg;
    cfg.drive = 1.0f;
    cfg.mix   = 0.0f;  // start bypassed
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, kChannels);

    auto buf1 = ConstantBuffer(64, 0.5f);
    sat.Process(buf1.data(), 64, kChannels, nullptr, 0);
    EXPECT(buf1[0] == 0.5f);  // bypassed

    sat.OnParameter(EffectParameter::Saturation_Drive,      4.0f);
    sat.OnParameter(EffectParameter::Saturation_Mix,        1.0f);
    sat.OnParameter(EffectParameter::Saturation_OutputGain, 1.0f);

    auto buf2 = ConstantBuffer(64, 0.5f);
    sat.Process(buf2.data(), 64, kChannels, nullptr, 0);
    const float expected = std::tanh(0.5f * 4.0f);
    constexpr uint32_t kSteadyStateFrame = 32;
    const float observed = buf2[kSteadyStateFrame * kChannels];
    std::printf("    after OnParameter drive=4 mix=1: steady-state=%.6f expected=%.6f\n",
                  observed, expected);
    EXPECT(std::abs(observed - expected) < 1e-6f);

    // Verify Mix clamps at 1.0 and Drive doesn't go negative.
    sat.OnParameter(EffectParameter::Saturation_Mix,   2.0f);   // >1
    sat.OnParameter(EffectParameter::Saturation_Drive, -3.0f);  // <0
    EXPECT(sat.Mix()   == 1.0f);
    EXPECT(sat.Drive() == 0.0f);
}

// =============================================================================
// 8. v0.38.0: ADAA aliasing reduction.
//
// Feed a 19 kHz sine at amplitude 0.7 through the shaper at drive=3.0,
// mix=1.0. tanh's odd harmonics that exceed Nyquist (24 kHz at 48 kHz SR)
// fold back into the audible band. The 9th harmonic of 19 kHz is
// 171 kHz, which folds to 21 kHz — squarely in our [20, 23] kHz alias
// band of interest. The 7th harmonic at 133 kHz folds to 11 kHz; the
// 5th at 95 kHz folds to 1 kHz; etc. ADAA suppresses high-order
// harmonics, so the 21 kHz alias from the 9th harmonic should be
// far below the fundamental at 19 kHz.
//
// Threshold: alias_energy / fundamental_energy < 0.01 (i.e. -40 dB).
// This is conservative; ADAA on tanh typically achieves -55 to -70 dB
// on this test. Leaving 15+ dB headroom in case of compiler / FP
// variation across platforms.
// =============================================================================
void TestADAASuppressesAliasing() {
    std::printf("  [v0.38.0: ADAA suppresses 19kHz × tanh aliasing]\n");
    SaturationConfig cfg;
    cfg.drive      = 3.0f;
    cfg.mix        = 1.0f;
    cfg.outputGain = 1.0f;
    cfg.bias       = 0.0f;
    SaturationEffect sat(cfg);
    sat.Prepare(kSampleRate, /*channels=*/1);

    // 1024 samples of a 19 kHz sine at amplitude 0.7, sampled at
    // 48 kHz. Mono for simpler DFT bookkeeping.
    constexpr uint32_t kFrames = 1024;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kFreq = 19000.0;
    constexpr double kAmp = 0.7;
    std::vector<float> buf(kFrames);
    for (uint32_t n = 0; n < kFrames; ++n) {
        buf[n] = static_cast<float>(kAmp * std::sin(
                2.0 * kPi * kFreq * n / static_cast<double>(kSampleRate)));
    }

    sat.Process(buf.data(), kFrames, /*channels=*/1, nullptr, 0);

    // DFT bin width = 48000 / 1024 = 46.875 Hz. So:
    //   19 kHz  → bin 405.33  (fundamental)
    //   20 kHz  → bin 426.67  (alias band start)
    //   23 kHz  → bin 490.67  (alias band end)
    constexpr double kBinHz = static_cast<double>(kSampleRate) / static_cast<double>(kFrames);

    const double fundamentalBin = kFreq / kBinHz;
    const double fundMag = DftMagnitudeAtBin(buf, fundamentalBin);

    // Scan the alias band [20 kHz .. 23 kHz] in integer bin steps and
    // take the worst (loudest) bin.
    const int aliasStartBin = static_cast<int>(std::ceil(20000.0 / kBinHz));
    const int aliasEndBin   = static_cast<int>(std::floor(23000.0 / kBinHz));
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
    std::printf("    fundamental @ bin %.2f (≈%.0f Hz): mag=%.6f\n",
                  fundamentalBin, kFreq, fundMag);
    std::printf("    worst alias @ bin %d (≈%.0f Hz): mag=%.6f\n",
                  worstAliasBin, worstAliasBin * kBinHz, worstAliasMag);
    std::printf("    alias / fundamental = %.6f (%.1f dB)\n", ratio, ratio_db);

    // Conservative threshold — see header comment.
    EXPECT(ratio < 0.01);
    EXPECT(fundMag > 100.0);  // fundamental must actually be present
}

} // namespace

int main() {
    std::printf("[saturation_test]\n");
    TestBypassIsIdentity();
    TestUnityDriveMatchesTanh();
    TestDriveCompressesPeaks();
    TestBiasDoesNotIntroduceDc();
    TestMixInterpolatesLinearly();
    TestSymmetryWithoutBias();
    TestRuntimeParameterChanges();
    TestADAASuppressesAliasing();
    std::printf("[saturation_test] PASSED\n");
    return 0;
}
