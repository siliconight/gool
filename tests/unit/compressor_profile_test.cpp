// tests/unit/compressor_profile_test.cpp
//
// Verifies the curated compression profiles in
// `audio_engine/compressor_profiles.h`. Two layers of check per
// profile:
//
//   1. Descriptor sanity: the constexpr function returns an
//      EffectConfig with the documented field values and
//      kind == Compressor. Catches accidental drift if someone
//      "tweaks" a profile and forgets to update the doc, or vice
//      versa.
//
//   2. Audibility smoke: instantiate a CompressorEffect from the
//      profile (translating EffectConfig -> CompressorConfig the
//      same way bus_graph.cpp does at runtime), push a known signal
//      through, verify reduction is non-negative and finite. Catches
//      any profile that compiles but silently produces garbage —
//      e.g. ratio = 0, or sidechainHpfHz beyond Nyquist.
//
// The DSP correctness for each parameter is already covered by
// compressor_test.cpp; we don't repeat that here.

#include "audio_engine/compressor_profiles.h"
#include "audio_engine/dsp/compressor.h"
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

// Mirrors bus_graph.cpp's EffectConfig -> CompressorConfig translation.
// Kept private to this test file because hosts shouldn't construct
// effects directly; they go through bus.effects + the runtime.
CompressorConfig ToRuntimeConfig(const EffectConfig& ec) {
    CompressorConfig cc;
    cc.thresholdDb     = ec.compressorThresholdDb;
    cc.ratio           = ec.compressorRatio;
    cc.attackMs        = ec.compressorAttackMs;
    cc.releaseMs       = ec.compressorReleaseMs;
    cc.makeupDb        = ec.compressorMakeupDb;
    cc.sidechainBus    = ec.compressorSidechainBus;
    cc.kneeWidthDb     = ec.compressorKneeWidthDb;
    cc.mixRatio        = ec.compressorMixRatio;
    cc.maxReductionDb  = ec.compressorMaxReductionDb;
    cc.sidechainHpfHz  = ec.compressorSidechainHpfHz;
    cc.holdMs          = ec.compressorHoldMs;
    cc.detectionMode   = ec.compressorDetectionMode;
    return cc;
}

// Run a constant-amplitude signal through a compressor built from
// the profile. Returns the reduction (in dB) at end of buffer.
float SmokeRunReduction(const EffectConfig& ec, float inputLevel) {
    CompressorEffect fx(ToRuntimeConfig(ec));
    fx.Prepare(kSampleRate, kChannels);
    constexpr uint32_t frames = 4800;  // 100 ms — enough for envelope to settle
    std::vector<float> buf(frames * kChannels, inputLevel);
    fx.Process(buf.data(), frames, kChannels, nullptr, 0);
    return fx.CurrentReductionDb();
}

// =============================================================================
// PUNCH
// =============================================================================

void TestDrumBusPunchProfile() {
    std::printf("  [profile] DrumBusPunch:\n");
    auto ec = CompressorProfiles::DrumBusPunch();
    EXPECT(ec.kind == EffectKind::Compressor);
    EXPECT(ec.compressorThresholdDb     == -18.0f);
    EXPECT(ec.compressorRatio           == 4.0f);
    EXPECT(ec.compressorAttackMs        == 10.0f);
    EXPECT(ec.compressorReleaseMs       == 80.0f);
    EXPECT(ec.compressorMakeupDb        == 2.0f);
    EXPECT(ec.compressorKneeWidthDb     == 6.0f);
    EXPECT(ec.compressorMixRatio        == 0.7f);
    EXPECT(ec.compressorDetectionMode   ==
           EffectConfig::CompressorDetectionMode::Rms);
    // Threshold override propagates.
    auto ec2 = CompressorProfiles::DrumBusPunch(-12.0f);
    EXPECT(ec2.compressorThresholdDb == -12.0f);
    EXPECT(ec2.compressorRatio == 4.0f);  // others unchanged
    // Audibility: drive with -10 dB signal, expect some reduction.
    const float reduction = SmokeRunReduction(ec, 0.3162f);  // -10 dB
    std::printf("    reduction at -10 dB input: %.2f dB\n", reduction);
    EXPECT(std::isfinite(reduction));
    EXPECT(reduction >= 0.0f);
    EXPECT(reduction < 60.0f);
    std::printf("    OK\n");
}

void TestFootstepGlueProfile() {
    std::printf("  [profile] FootstepGlue:\n");
    auto ec = CompressorProfiles::FootstepGlue();
    EXPECT(ec.kind == EffectKind::Compressor);
    EXPECT(ec.compressorThresholdDb     == -22.0f);
    EXPECT(ec.compressorRatio           == 3.0f);
    EXPECT(ec.compressorAttackMs        == 8.0f);
    EXPECT(ec.compressorReleaseMs       == 60.0f);
    EXPECT(ec.compressorMakeupDb        == 1.0f);
    EXPECT(ec.compressorKneeWidthDb     == 4.0f);
    EXPECT(ec.compressorMixRatio        == 0.6f);
    EXPECT(ec.compressorDetectionMode   ==
           EffectConfig::CompressorDetectionMode::Peak);
    const float reduction = SmokeRunReduction(ec, 0.1995f);  // -14 dB
    std::printf("    reduction at -14 dB input: %.2f dB\n", reduction);
    EXPECT(std::isfinite(reduction));
    EXPECT(reduction >= 0.0f);
    std::printf("    OK\n");
}

void TestGunshotSnapProfile() {
    std::printf("  [profile] GunshotSnap:\n");
    auto ec = CompressorProfiles::GunshotSnap();
    EXPECT(ec.kind == EffectKind::Compressor);
    EXPECT(ec.compressorThresholdDb     == -16.0f);
    EXPECT(ec.compressorRatio           == 4.0f);
    EXPECT(ec.compressorAttackMs        == 5.0f);
    EXPECT(ec.compressorReleaseMs       == 100.0f);
    EXPECT(ec.compressorMaxReductionDb  == 8.0f);   // range cap
    EXPECT(ec.compressorMixRatio        == 0.8f);
    // Drive a very loud signal; verify reduction is capped at 8 dB.
    const float reduction = SmokeRunReduction(ec, 1.0f);  // 0 dBFS
    std::printf("    reduction at 0 dBFS input: %.2f dB (capped at 8)\n", reduction);
    EXPECT(std::isfinite(reduction));
    EXPECT(reduction <= 8.01f);  // tiny epsilon for float
    std::printf("    OK\n");
}

// =============================================================================
// IMPACT
// =============================================================================

void TestExplosionImpactProfile() {
    std::printf("  [profile] ExplosionImpact:\n");
    auto ec = CompressorProfiles::ExplosionImpact();
    EXPECT(ec.kind == EffectKind::Compressor);
    EXPECT(ec.compressorThresholdDb     == -14.0f);
    EXPECT(ec.compressorRatio           == 5.0f);
    EXPECT(ec.compressorAttackMs        == 3.0f);
    EXPECT(ec.compressorReleaseMs       == 150.0f);
    EXPECT(ec.compressorKneeWidthDb     == 3.0f);
    EXPECT(ec.compressorMaxReductionDb  == 12.0f);
    EXPECT(ec.compressorDetectionMode   ==
           EffectConfig::CompressorDetectionMode::Peak);
    const float reduction = SmokeRunReduction(ec, 0.5012f);  // -6 dB
    std::printf("    reduction at -6 dB input: %.2f dB\n", reduction);
    EXPECT(std::isfinite(reduction));
    EXPECT(reduction <= 12.01f);
    std::printf("    OK\n");
}

void TestBassImpactProfile() {
    std::printf("  [profile] BassImpact:\n");
    auto ec = CompressorProfiles::BassImpact();
    EXPECT(ec.kind == EffectKind::Compressor);
    EXPECT(ec.compressorThresholdDb     == -20.0f);
    EXPECT(ec.compressorRatio           == 3.0f);
    EXPECT(ec.compressorAttackMs        == 15.0f);
    EXPECT(ec.compressorReleaseMs       == 200.0f);
    EXPECT(ec.compressorKneeWidthDb     == 8.0f);
    EXPECT(ec.compressorSidechainHpfHz  == 80.0f);  // ignore own sub
    const float reduction = SmokeRunReduction(ec, 0.2512f);  // -12 dB
    std::printf("    reduction at -12 dB input (post-HPF): %.2f dB\n", reduction);
    EXPECT(std::isfinite(reduction));
    EXPECT(reduction >= 0.0f);
    std::printf("    OK\n");
}

// =============================================================================
// DYNAMICS / GLUE
// =============================================================================

void TestMasterBusGlueProfile() {
    std::printf("  [profile] MasterBusGlue:\n");
    auto ec = CompressorProfiles::MasterBusGlue();
    EXPECT(ec.kind == EffectKind::Compressor);
    EXPECT(ec.compressorThresholdDb     == -10.0f);
    EXPECT(ec.compressorRatio           == 1.5f);   // very gentle
    EXPECT(ec.compressorAttackMs        == 30.0f);
    EXPECT(ec.compressorReleaseMs       == 250.0f);
    EXPECT(ec.compressorMakeupDb        == 0.5f);
    EXPECT(ec.compressorKneeWidthDb     == 8.0f);
    EXPECT(ec.compressorDetectionMode   ==
           EffectConfig::CompressorDetectionMode::Rms);
    // At -6 dB input, expect very mild reduction (1.5:1 over 4 dB
    // overshoot = ~1.3 dB reduction).
    const float reduction = SmokeRunReduction(ec, 0.5012f);  // -6 dB
    std::printf("    reduction at -6 dB input: %.2f dB (gentle)\n", reduction);
    EXPECT(std::isfinite(reduction));
    EXPECT(reduction >= 0.0f);
    EXPECT(reduction < 5.0f);  // very gentle by design
    std::printf("    OK\n");
}

void TestVoiceSmoothingProfile() {
    std::printf("  [profile] VoiceSmoothing:\n");
    auto ec = CompressorProfiles::VoiceSmoothing();
    EXPECT(ec.kind == EffectKind::Compressor);
    EXPECT(ec.compressorThresholdDb     == -18.0f);
    EXPECT(ec.compressorRatio           == 4.0f);
    EXPECT(ec.compressorAttackMs        == 5.0f);
    EXPECT(ec.compressorReleaseMs       == 80.0f);
    EXPECT(ec.compressorMakeupDb        == 1.5f);
    EXPECT(ec.compressorKneeWidthDb     == 6.0f);
    EXPECT(ec.compressorHoldMs          == 30.0f);
    EXPECT(ec.compressorDetectionMode   ==
           EffectConfig::CompressorDetectionMode::Rms);
    const float reduction = SmokeRunReduction(ec, 0.1259f);  // -18 dB (at threshold)
    std::printf("    reduction at -18 dB input (at threshold): %.2f dB\n", reduction);
    EXPECT(std::isfinite(reduction));
    EXPECT(reduction >= 0.0f);
    std::printf("    OK\n");
}

// =============================================================================
// SIDECHAIN DUCKERS
// =============================================================================

void TestMusicDuckUnderVoiceProfile() {
    std::printf("  [profile] MusicDuckUnderVoice:\n");
    auto ec = CompressorProfiles::MusicDuckUnderVoice();
    EXPECT(ec.kind == EffectKind::Compressor);
    EXPECT(ec.compressorThresholdDb     == -22.0f);
    EXPECT(ec.compressorRatio           == 8.0f);   // assertive
    EXPECT(ec.compressorAttackMs        == 5.0f);
    EXPECT(ec.compressorReleaseMs       == 200.0f);
    EXPECT(ec.compressorKneeWidthDb     == 6.0f);
    EXPECT(ec.compressorMaxReductionDb  == 12.0f);
    EXPECT(ec.compressorSidechainHpfHz  == 200.0f); // ignore breath/pop
    EXPECT(ec.compressorDetectionMode   ==
           EffectConfig::CompressorDetectionMode::Rms);
    // sidechainBus left as default — host must wire.
    EXPECT(ec.compressorSidechainBus == kInvalidBusId);
    // Use main signal as detect (self-sidechain in this smoke test —
    // real usage wires a separate trigger bus).
    const float reduction = SmokeRunReduction(ec, 0.1995f);  // -14 dB
    std::printf("    reduction at -14 dB self-trigger: %.2f dB (capped at 12)\n", reduction);
    EXPECT(std::isfinite(reduction));
    EXPECT(reduction <= 12.01f);
    std::printf("    OK\n");
}

void TestMusicDuckUnderSfxProfile() {
    std::printf("  [profile] MusicDuckUnderSfx:\n");
    auto ec = CompressorProfiles::MusicDuckUnderSfx();
    EXPECT(ec.kind == EffectKind::Compressor);
    EXPECT(ec.compressorThresholdDb     == -18.0f);
    EXPECT(ec.compressorRatio           == 6.0f);
    EXPECT(ec.compressorAttackMs        == 8.0f);
    EXPECT(ec.compressorReleaseMs       == 250.0f);
    EXPECT(ec.compressorKneeWidthDb     == 4.0f);
    EXPECT(ec.compressorMaxReductionDb  == 9.0f);
    EXPECT(ec.compressorSidechainHpfHz  == 150.0f);
    EXPECT(ec.compressorDetectionMode   ==
           EffectConfig::CompressorDetectionMode::Rms);
    EXPECT(ec.compressorSidechainBus == kInvalidBusId);
    const float reduction = SmokeRunReduction(ec, 0.3162f);  // -10 dB
    std::printf("    reduction at -10 dB self-trigger: %.2f dB (capped at 9)\n", reduction);
    EXPECT(std::isfinite(reduction));
    EXPECT(reduction <= 9.01f);
    std::printf("    OK\n");
}

// =============================================================================
// Cross-cut sanity: every profile sets kind = Compressor, doesn't
// touch unrelated fields, returns the same struct on repeated call.
// =============================================================================

void TestProfilesAreSelfContained() {
    std::printf("  [profile] cross-cut:\n");

    // Determinism: calling a profile twice returns identical structs.
    auto a1 = CompressorProfiles::DrumBusPunch();
    auto a2 = CompressorProfiles::DrumBusPunch();
    EXPECT(a1.compressorThresholdDb == a2.compressorThresholdDb);
    EXPECT(a1.compressorRatio       == a2.compressorRatio);

    // Profiles don't touch unrelated effect fields — they should
    // remain at their EffectConfig defaults so a host could in
    // principle reuse the descriptor for another effect kind by
    // changing `kind` (although that would be weird).
    auto v = CompressorProfiles::VoiceSmoothing();
    EXPECT(v.gainDb         == 0.0f);
    EXPECT(v.biquadCutoffHz == 20000.0f);
    EXPECT(v.reverbDecay    == 0.5f);   // v0.29.0 Dattorro default

    std::printf("    OK\n");
}

} // namespace

int main() {
    std::printf("[compressor_profile_test]\n");

    TestDrumBusPunchProfile();
    TestFootstepGlueProfile();
    TestGunshotSnapProfile();

    TestExplosionImpactProfile();
    TestBassImpactProfile();

    TestMasterBusGlueProfile();
    TestVoiceSmoothingProfile();

    TestMusicDuckUnderVoiceProfile();
    TestMusicDuckUnderSfxProfile();

    TestProfilesAreSelfContained();

    std::printf("[compressor_profile_test] PASSED\n");
    return 0;
}
