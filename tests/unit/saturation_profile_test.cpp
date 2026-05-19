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
    EXPECT(ec.saturationDrive      == 1.5f);
    EXPECT(ec.saturationMix        == 0.15f);
    EXPECT(ec.saturationOutputGain == 0.85f);
    EXPECT(ec.saturationBias       == 0.0f);
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
    EXPECT(ec.saturationDrive      == 1.3f);
    EXPECT(ec.saturationMix        == 0.10f);
    EXPECT(ec.saturationOutputGain == 0.9f);
    EXPECT(ec.saturationBias       == 0.05f);
    // Asymmetric: bias != 0 means the DC-removal path runs. Verify
    // a zero input still produces zero output (DC removal works
    // for the profile, not just for the unit test in saturation_test).
    SaturationEffect fx(ToRuntimeConfig(ec));
    fx.Prepare(kSampleRate, kChannels);
    std::vector<float> buf(256 * kChannels, 0.0f);
    fx.Process(buf.data(), 256, kChannels, nullptr, 0);
    float maxAbs = 0.0f;
    for (float v : buf) maxAbs = std::max(maxAbs, std::abs(v));
    std::printf("    zero input → max|output|: %.8f (expect ≈ 0)\n", maxAbs);
    EXPECT(maxAbs < 1e-6f);
}

void TestWeaponBodyProfile() {
    std::printf("  [profile] WeaponBody:\n");
    auto ec = SaturationProfiles::WeaponBody();
    EXPECT(ec.kind                 == EffectKind::Saturation);
    EXPECT(ec.saturationDrive      == 2.5f);
    EXPECT(ec.saturationMix        == 0.30f);
    EXPECT(ec.saturationOutputGain == 0.7f);
    EXPECT(ec.saturationBias       == 0.0f);
    const float p = SmokePeakAfterSaturation(ec, 0.7f);
    std::printf("    peak after 0.7 input: %.4f (saturated body)\n", p);
    EXPECT(p > 0.0f && p < 1.0f);
}

void TestImpactCharacterProfile() {
    std::printf("  [profile] ImpactCharacter:\n");
    auto ec = SaturationProfiles::ImpactCharacter();
    EXPECT(ec.kind                 == EffectKind::Saturation);
    EXPECT(ec.saturationDrive      == 4.0f);
    EXPECT(ec.saturationMix        == 0.45f);
    EXPECT(ec.saturationOutputGain == 0.55f);
    EXPECT(ec.saturationBias       == 0.10f);
    const float p = SmokePeakAfterSaturation(ec, 0.8f);
    std::printf("    peak after 0.8 input: %.4f (heavy harmonic content)\n", p);
    EXPECT(p > 0.0f && p < 1.0f);
}

void TestTapeColorProfile() {
    std::printf("  [profile] TapeColor:\n");
    auto ec = SaturationProfiles::TapeColor();
    EXPECT(ec.kind                 == EffectKind::Saturation);
    EXPECT(ec.saturationDrive      == 2.0f);
    EXPECT(ec.saturationMix        == 0.25f);
    EXPECT(ec.saturationOutputGain == 0.75f);
    EXPECT(ec.saturationBias       == 0.0f);
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
