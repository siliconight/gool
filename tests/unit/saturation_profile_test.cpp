// tests/unit/saturation_profile_test.cpp
//
// Verifies the curated saturation profiles. Same shape as
// compressor_profile_test.cpp: sanity check the documented constants
// haven't drifted, plus a smoke audibility test that instantiates a
// SaturationEffect from each profile and pushes a known signal
// through.

#include "audio_engine/saturation_profiles.h"
#include "audio_engine/dsp/saturation_effect.h"
#include "audio_engine/bus.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// Mirrors bus_graph.cpp's EffectConfig -> SaturationConfig translation.
SaturationConfig ToRuntimeConfig(const EffectConfig& ec) {
    SaturationConfig sc;
    sc.drive      = ec.saturationDrive;
    sc.mix        = ec.saturationMix;
    sc.outputGain = ec.saturationOutputGain;
    sc.bias       = ec.saturationBias;
    // v0.40.0: saturationMode is a new uint8_t field on EffectConfig
    // (default 0 = Tanh, matching pre-v0.40.0 behavior). Mirror the
    // defensive range check from bus_graph.cpp here so a corrupt
    // value falls back to Tanh rather than UB.
    sc.mode = (ec.saturationMode <= 3)
                ? static_cast<SaturationMode>(ec.saturationMode)
                : SaturationMode::Tanh;
    return sc;
}

// Drive a known constant signal through a saturator built from the
// profile. Returns peak |output| over the buffer.
float SmokePeakAfterSaturation(const EffectConfig& ec, float inputLevel) {
    SaturationEffect fx(ToRuntimeConfig(ec));
    fx.Prepare(kSampleRate, kChannels);
    constexpr uint32_t frames = 256;
    std::vector<float> buf(frames * kChannels, inputLevel);
    fx.Process(buf.data(), frames, kChannels, nullptr, 0);
    float peak = 0.0f;
    for (float v : buf) peak = std::max(peak, std::abs(v));
    return peak;
}

void TestBusGlueProfile() {
    std::printf("  [profile] BusGlue:\n");
    auto ec = SaturationProfiles::BusGlue();
    EXPECT(ec.kind                 == EffectKind::Saturation);
    // v0.40.0: profile now uses normalized 0..1 drive. 0.1667 maps to
    // Tanh scale 1.5 = identical sound to pre-v0.40.0 drive=1.5.
    EXPECT(std::abs(ec.saturationDrive - 0.1667f) < 1e-4f);
    EXPECT(ec.saturationMix        == 0.15f);
    EXPECT(ec.saturationOutputGain == 0.85f);
    EXPECT(ec.saturationBias       == 0.0f);
    EXPECT(ec.saturationMode       == 0);   // Tanh default
    const float p = SmokePeakAfterSaturation(ec, 0.5f);
    std::printf("    peak after 0.5 input: %.4f (subtle change expected)\n", p);
    // Light glue: output should be very close to input (15 % wet,
    // gentle drive, output-trimmed). Bound the change loosely.
    EXPECT(p > 0.4f && p < 0.6f);
}

void TestDialogueWarmthProfile() {
    std::printf("  [profile] DialogueWarmth:\n");
    auto ec = SaturationProfiles::DialogueWarmth();
    EXPECT(ec.kind                 == EffectKind::Saturation);
    // v0.40.0: norm drive 0.10 = Tanh scale 1.3 (round-trip from pre-
    // v0.40.0 drive=1.3).
    EXPECT(std::abs(ec.saturationDrive - 0.10f) < 1e-4f);
    EXPECT(ec.saturationMix        == 0.10f);
    EXPECT(ec.saturationOutputGain == 0.9f);
    EXPECT(ec.saturationBias       == 0.05f);
    EXPECT(ec.saturationMode       == 0);   // Tanh default
    // Asymmetric: bias != 0 means the DC-removal path runs. Verify
    // a zero input still produces zero output at steady-state (DC
    // removal works for the profile, not just for the unit test in
    // saturation_test).
    //
    // v0.38.0: ADAA's first sample on a zero buffer uses the cold-
    // start state x[n-1]=0, but the new sample x[n]=bias*drive is
    // nonzero — so the first sample's ADAA output differs slightly
    // from the steady-state tanh(bias*drive) that the dcRemove
    // subtraction is calibrated to. Result: one sample of small
    // residue per channel before steady-state takes over. Measure
    // max|output| over the steady-state region only (skip frame 0).
    SaturationEffect fx(ToRuntimeConfig(ec));
    fx.Prepare(kSampleRate, kChannels);
    std::vector<float> buf(256 * kChannels, 0.0f);
    fx.Process(buf.data(), 256, kChannels, nullptr, 0);
    constexpr size_t kSkipFrames = 4;
    float maxAbs = 0.0f;
    for (size_t i = kSkipFrames * kChannels; i < buf.size(); ++i) {
        maxAbs = std::max(maxAbs, std::abs(buf[i]));
    }
    std::printf("    zero input → steady-state max|output|: %.8f (expect ≈ 0)\n", maxAbs);
    EXPECT(maxAbs < 1e-6f);
}

void TestWeaponBodyProfile() {
    std::printf("  [profile] WeaponBody:\n");
    auto ec = SaturationProfiles::WeaponBody();
    EXPECT(ec.kind                 == EffectKind::Saturation);
    // v0.40.0: norm drive 0.5 = Tanh scale 2.5 (round-trip).
    EXPECT(std::abs(ec.saturationDrive - 0.5f) < 1e-4f);
    EXPECT(ec.saturationMix        == 0.30f);
    EXPECT(ec.saturationOutputGain == 0.7f);
    EXPECT(ec.saturationBias       == 0.0f);
    EXPECT(ec.saturationMode       == 0);   // Tanh default
    const float p = SmokePeakAfterSaturation(ec, 0.7f);
    std::printf("    peak after 0.7 input: %.4f (saturated body)\n", p);
    EXPECT(p > 0.0f && p < 1.0f);
}

void TestImpactCharacterProfile() {
    std::printf("  [profile] ImpactCharacter:\n");
    auto ec = SaturationProfiles::ImpactCharacter();
    EXPECT(ec.kind                 == EffectKind::Saturation);
    // v0.40.0: norm drive 1.0 = Tanh scale 4.0 (round-trip; max Tanh).
    EXPECT(std::abs(ec.saturationDrive - 1.0f) < 1e-4f);
    EXPECT(ec.saturationMix        == 0.45f);
    EXPECT(ec.saturationOutputGain == 0.55f);
    EXPECT(ec.saturationBias       == 0.10f);
    EXPECT(ec.saturationMode       == 0);   // Tanh default
    const float p = SmokePeakAfterSaturation(ec, 0.8f);
    std::printf("    peak after 0.8 input: %.4f (heavy harmonic content)\n", p);
    EXPECT(p > 0.0f && p < 1.0f);
}

void TestTapeColorProfile() {
    std::printf("  [profile] TapeColor:\n");
    auto ec = SaturationProfiles::TapeColor();
    EXPECT(ec.kind                 == EffectKind::Saturation);
    // v0.40.0: norm drive 0.3333 = Tanh scale 2.0 (round-trip).
    EXPECT(std::abs(ec.saturationDrive - 0.3333f) < 1e-4f);
    EXPECT(ec.saturationMix        == 0.25f);
    EXPECT(ec.saturationOutputGain == 0.75f);
    EXPECT(ec.saturationBias       == 0.0f);
    EXPECT(ec.saturationMode       == 0);   // Tanh default (despite the
                                            // name; see profile comment
                                            // re: SaturationMode::Tape)
    const float p = SmokePeakAfterSaturation(ec, 0.6f);
    std::printf("    peak after 0.6 input: %.4f\n", p);
    EXPECT(p > 0.0f && p < 1.0f);
}

void TestProfilesAreSelfContained() {
    std::printf("  [profile] cross-cut:\n");
    // Determinism.
    auto a1 = SaturationProfiles::BusGlue();
    auto a2 = SaturationProfiles::BusGlue();
    EXPECT(a1.saturationDrive == a2.saturationDrive);
    EXPECT(a1.saturationMix   == a2.saturationMix);

    // Profiles don't touch unrelated EffectConfig fields.
    auto v = SaturationProfiles::DialogueWarmth();
    EXPECT(v.gainDb              == 0.0f);
    EXPECT(v.compressorRatio     == 4.0f);   // EffectConfig default
    EXPECT(v.reverbDecay         == 0.5f);   // v0.29.0 Dattorro default
    EXPECT(v.compressorMixRatio  == 1.0f);

    std::printf("    OK\n");
}

} // namespace

int main() {
    std::printf("[saturation_profile_test]\n");
    TestBusGlueProfile();
    TestDialogueWarmthProfile();
    TestWeaponBodyProfile();
    TestImpactCharacterProfile();
    TestTapeColorProfile();
    TestProfilesAreSelfContained();
    std::printf("[saturation_profile_test] PASSED\n");
    return 0;
}
