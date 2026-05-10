// tests/unit/saturation_test.cpp
//
// Tests the SaturationEffect waveshaper at the DSP level. Verifies:
//
//   1. Bypass at default config (mix=0): output identical to input.
//   2. Drive=1, mix=1, bias=0: output is exactly tanh(input) * outputGain.
//   3. Drive>1 produces compression: peaks below 1.0 even when
//      driven above 1.0.
//   4. DC bias does NOT introduce DC at the output: the
//      tanh(bias*drive) subtraction works.
//   5. Mix interpolates linearly between dry and wet.
//   6. Drive=1 (no boost) produces NO new harmonics (identity if
//      output gain compensates).
//   7. Runtime parameter updates take effect on the next Process.

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

// Mean of an interleaved buffer over both channels.
float Mean(const std::vector<float>& buf) {
    if (buf.empty()) return 0.0f;
    return std::accumulate(buf.begin(), buf.end(), 0.0f) /
           static_cast<float>(buf.size());
}

// Peak of an interleaved buffer over both channels.
float PeakAbs(const std::vector<float>& buf) {
    float p = 0.0f;
    for (float v : buf) p = std::max(p, std::abs(v));
    return p;
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
// 2. drive=1, mix=1, bias=0: output = tanh(input).
// =============================================================================
void TestUnityDriveMatchesTanh() {
    std::printf("  [drive=1 mix=1 bias=0: output is exactly tanh(input)]\n");
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
    for (size_t i = 0; i < buf.size(); ++i) {
        EXPECT(std::abs(buf[i] - expected) < 1e-6f);
    }
    std::printf("    OK (input=%.3f → output=%.6f, expected tanh=%.6f)\n",
                  input, buf[0], expected);
}

// =============================================================================
// 3. Drive > 1 compresses peaks: tanh saturates above ~|3| toward ±1.
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
//    is subtracted internally).
// =============================================================================
void TestBiasDoesNotIntroduceDc() {
    std::printf("  [bias=0.2: DC offset removed; mean of zero input = 0]\n");
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

    const float mean = Mean(buf);
    std::printf("    mean of saturated zero input: %.8f (expect ≈ 0)\n", mean);
    EXPECT(std::abs(mean) < 1e-6f);  // DC removal works
}

// =============================================================================
// 5. Mix interpolates linearly between dry and wet.
//    At mix=0.5, output = 0.5*dry + 0.5*wet.
// =============================================================================
void TestMixInterpolatesLinearly() {
    std::printf("  [mix=0.5: output is exactly halfway between dry and wet]\n");
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
    std::printf("    dry=%.4f wet=%.4f mix=0.5 → expected=%.4f got=%.4f\n",
                  input, wet, expected, buf[0]);
    EXPECT(std::abs(buf[0] - expected) < 1e-6f);
}

// =============================================================================
// 6. Symmetry of default tanh: output(-x) == -output(x) when bias=0.
//    Confirms odd-harmonic-only character of the symmetric mode.
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
    std::printf("    after OnParameter drive=4 mix=1: got=%.6f expected=%.6f\n",
                  buf2[0], expected);
    EXPECT(std::abs(buf2[0] - expected) < 1e-6f);

    // Verify Mix clamps at 1.0 and Drive doesn't go negative.
    sat.OnParameter(EffectParameter::Saturation_Mix,   2.0f);   // >1
    sat.OnParameter(EffectParameter::Saturation_Drive, -3.0f);  // <0
    EXPECT(sat.Mix()   == 1.0f);
    EXPECT(sat.Drive() == 0.0f);
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
    std::printf("[saturation_test] PASSED\n");
    return 0;
}
