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

// tests/unit/bus_config_loader_test.cpp
//
// Pure C++ tests for the JSON → BusGraphConfig translator. No
// Godot dependency. Verifies:
//
//   1. Minimal valid config parses (single Master bus).
//   2. Multi-tier ducking config (Master / SfxAll / LocalSfx /
//      RemoteSfx / Music) parses with correct parent + sidechain
//      references resolved by name.
//   3. Every effect kind round-trips its fields.
//   4. category_routing maps to correct BusIds.
//   5. Malformed JSON returns ok=false with line number.
//   6. Unknown effect kind returns descriptive error.
//   7. Unresolved bus reference (parent or sidechain_bus) returns
//      descriptive error.
//   8. The parsed BusGraphConfig actually initializes a real
//      AudioRuntime — i.e. the engine accepts what we produced.

#include "audio_engine/bus_config_loader.h"
#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/backend/null_audio_backend.h"
#include "audio_engine/bus.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

using namespace audio;

namespace {

#define EXPECT(cond) do {                                                       \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL: %s @ %s:%d\n", #cond, __FILE__, __LINE__); \
            std::abort();                                                       \
        }                                                                       \
    } while (0)

#define EXPECT_OK(r) do {                                                       \
        if (!(r).ok) {                                                          \
            std::fprintf(stderr, "FAIL: parse error '%s' on line %d @ %s:%d\n", \
                          (r).error.c_str(), (r).errorLine, __FILE__, __LINE__); \
            std::abort();                                                       \
        }                                                                       \
    } while (0)

// =============================================================================
// 1. Minimal config: single Master.
// =============================================================================
void TestMinimalConfig() {
    std::printf("  [minimal config: single Master bus]\n");
    constexpr std::string_view json = R"({
        "buses": [
            { "name": "Master", "gain_db": 0.0 }
        ]
    })";
    auto r = BusConfigLoader::ParseFromJson(json);
    EXPECT_OK(r);
    EXPECT(r.busGraph.busCount == 1);
    EXPECT(r.busGraph.buses[kBusMaster].id == kBusMaster);
    EXPECT(r.busGraph.buses[kBusMaster].parent == kBusMaster);
    EXPECT(r.busGraph.buses[kBusMaster].outputGainDb == 0.0f);
    EXPECT(r.busGraph.buses[kBusMaster].effectCount == 0);
    std::printf("    OK (1 bus, no effects)\n");
}

// =============================================================================
// 2. Multi-tier ducking shape from examples/multi_tier_ducking.
// =============================================================================
void TestMultiTierDuckingShape() {
    std::printf("  [multi-tier ducking: Master/Music/SfxAll/Local/Remote]\n");
    // The exact L4D2-style shape: music ducks under LocalSfx, and
    // RemoteSfx also ducks under LocalSfx. The local player's gun
    // therefore wins over both music AND teammates' guns.
    constexpr std::string_view json = R"({
        "buses": [
            { "name": "Master",   "gain_db": 0.0 },
            { "name": "Music",    "parent": "Master", "gain_db": -3.0,
              "effects": [
                { "kind": "compressor",
                  "threshold_db": -30.0, "ratio": 8.0,
                  "attack_ms": 5.0, "release_ms": 250.0,
                  "sidechain_bus": "LocalSfx" }
              ] },
            { "name": "SfxAll",   "parent": "Master" },
            { "name": "LocalSfx", "parent": "SfxAll" },
            { "name": "RemoteSfx","parent": "SfxAll",
              "effects": [
                { "kind": "compressor",
                  "threshold_db": -30.0, "ratio": 8.0,
                  "attack_ms": 5.0, "release_ms": 250.0,
                  "sidechain_bus": "LocalSfx" }
              ] }
        ],
        "category_routing": {
            "music": "Music",
            "sfx":   "LocalSfx"
        }
    })";
    auto r = BusConfigLoader::ParseFromJson(json);
    EXPECT_OK(r);
    EXPECT(r.busGraph.busCount == 5);

    // Master at id 0.
    EXPECT(r.busGraph.buses[kBusMaster].id == kBusMaster);

    // Find the buses by their debug names. Their assigned IDs are
    // declaration-order with Master pinned to kBusMaster.
    auto findIdByName = [&](const char* name) -> BusId {
        for (uint32_t i = 0; i < r.busGraph.busCount; ++i) {
            if (std::string(r.busGraph.buses[i].debugName) == name) {
                return r.busGraph.buses[i].id;
            }
        }
        return kInvalidBusId;
    };
    BusId music     = findIdByName("Music");
    BusId sfxAll    = findIdByName("SfxAll");
    BusId localSfx  = findIdByName("LocalSfx");
    BusId remoteSfx = findIdByName("RemoteSfx");
    EXPECT(music     != kInvalidBusId);
    EXPECT(sfxAll    != kInvalidBusId);
    EXPECT(localSfx  != kInvalidBusId);
    EXPECT(remoteSfx != kInvalidBusId);

    // Parent relationships.
    EXPECT(r.busGraph.buses[music].parent     == kBusMaster);
    EXPECT(r.busGraph.buses[sfxAll].parent    == kBusMaster);
    EXPECT(r.busGraph.buses[localSfx].parent  == sfxAll);
    EXPECT(r.busGraph.buses[remoteSfx].parent == sfxAll);

    // Compressor on Music should sidechain to LocalSfx.
    EXPECT(r.busGraph.buses[music].effectCount == 1);
    EXPECT(r.busGraph.buses[music].effects[0].kind == EffectKind::Compressor);
    EXPECT(r.busGraph.buses[music].effects[0].compressorSidechainBus == localSfx);
    EXPECT(r.busGraph.buses[music].effects[0].compressorRatio == 8.0f);

    // Compressor on RemoteSfx should also sidechain to LocalSfx.
    EXPECT(r.busGraph.buses[remoteSfx].effectCount == 1);
    EXPECT(r.busGraph.buses[remoteSfx].effects[0].compressorSidechainBus == localSfx);

    // Category routing.
    EXPECT(r.busGraph.categoryMap.music == music);
    EXPECT(r.busGraph.categoryMap.sfx   == localSfx);
    // Unmapped categories stay at master (struct default).
    EXPECT(r.busGraph.categoryMap.voice    == kBusMaster);
    EXPECT(r.busGraph.categoryMap.dialogue == kBusMaster);

    std::printf("    OK (multi-tier sidechain refs resolved by name)\n");
}

// =============================================================================
// 3. All effect kinds parse their fields.
// =============================================================================
void TestAllEffectKinds() {
    std::printf("  [all 5 effect kinds parse their fields]\n");
    constexpr std::string_view json = R"({
        "buses": [
            { "name": "Master",
              "effects": [
                { "kind": "gain",       "gain_db": -2.0 },
                { "kind": "biquad",     "biquad_type": "highshelf",
                  "cutoff_hz": 8000.0, "q": 0.7, "biquad_gain_db": 3.0 },
                { "kind": "compressor", "threshold_db": -18.0, "ratio": 4.0,
                  "attack_ms": 10.0,    "release_ms": 100.0,
                  "knee_width_db": 6.0, "mix_ratio": 0.7,
                  "max_reduction_db": 12.0, "sidechain_hpf_hz": 150.0,
                  "hold_ms": 30.0,      "detection_mode": "rms" },
                { "kind": "reverb",     "room_size": 0.6, "damping": 0.4, "wet_gain_db": -2.0 },
                { "kind": "saturation", "drive": 1.5, "mix": 0.15,
                  "output_gain": 0.85,  "bias": 0.05 }
              ] }
        ]
    })";
    auto r = BusConfigLoader::ParseFromJson(json);
    EXPECT_OK(r);
    const auto& m = r.busGraph.buses[kBusMaster];
    EXPECT(m.effectCount == 5);

    EXPECT(m.effects[0].kind == EffectKind::Gain);
    EXPECT(m.effects[0].gainDb == -2.0f);

    EXPECT(m.effects[1].kind == EffectKind::BiquadFilter);
    EXPECT(m.effects[1].biquadType == BiquadType::HighShelf);
    EXPECT(m.effects[1].biquadCutoffHz == 8000.0f);
    EXPECT(m.effects[1].biquadGainDb == 3.0f);

    EXPECT(m.effects[2].kind == EffectKind::Compressor);
    EXPECT(m.effects[2].compressorThresholdDb     == -18.0f);
    EXPECT(m.effects[2].compressorKneeWidthDb     == 6.0f);
    EXPECT(m.effects[2].compressorMixRatio        == 0.7f);
    EXPECT(m.effects[2].compressorMaxReductionDb  == 12.0f);
    EXPECT(m.effects[2].compressorSidechainHpfHz  == 150.0f);
    EXPECT(m.effects[2].compressorHoldMs          == 30.0f);
    EXPECT(m.effects[2].compressorDetectionMode   ==
           EffectConfig::CompressorDetectionMode::Rms);

    EXPECT(m.effects[3].kind == EffectKind::Reverb);
    // v0.29.0 soft migration: the test JSON still uses the legacy
    // `room_size` and `damping` keys, which the loader now routes
    // into the new reverbDecay / reverbHfDamping fields. The numeric
    // mapping is 1:1, so the assertion values are unchanged from
    // v0.28.x — only the field names moved.
    EXPECT(m.effects[3].reverbDecay     == 0.6f);
    EXPECT(m.effects[3].reverbHfDamping == 0.4f);

    EXPECT(m.effects[4].kind == EffectKind::Saturation);
    // v0.40.0 soft migration: the legacy JSON value `"drive": 1.5` is
    // unnormalized (pre-v0.40.0 drives lived in [1..N]); the loader
    // detects drive > 1.0 and remaps it to the normalized form via
    // (raw - 1) / 3. (1.5 - 1) / 3 = 0.1666..., which on the default
    // Tanh mode maps back to scale 1.5 — so the round-tripped sound
    // is identical to pre-v0.40.0 (within FP rounding). The other
    // four fields pass through unchanged. Mode defaults to 0 (Tanh).
    EXPECT(std::abs(m.effects[4].saturationDrive - 0.1666667f) < 1e-6f);
    EXPECT(m.effects[4].saturationMix        == 0.15f);
    EXPECT(m.effects[4].saturationOutputGain == 0.85f);
    EXPECT(m.effects[4].saturationBias       == 0.05f);
    EXPECT(m.effects[4].saturationMode       == 0);   // default Tanh

    std::printf("    OK (all effect kinds + their fields)\n");
}

// =============================================================================
// v0.40.0: Saturation mode key + drive normalization soft-migration.
//
// Covers three behaviors introduced by Phase 2 of saturation_v2.md:
//   a) Legacy unnormalized drive (> 1.0) is silently mapped to
//      normalized 0..1 via the Tanh-mode inverse.
//   b) Already-normalized drive (≤ 1.0) is passed through unchanged.
//   c) New `mode` JSON key accepts "tanh"/"tube"/"tape"/"diode"
//      strings and rejects unknowns.
// =============================================================================
void TestSaturationModeAndMigration() {
    std::printf("  [v0.40.0: saturation mode key + drive normalization]\n");

    // a) Legacy unnormalized drive triggers soft migration.
    //    legacy 4.0 → norm 1.0 (saturates the Tanh range exactly).
    //    legacy 2.5 → norm 0.5 (mid-range, round-trip exact).
    //    legacy 99.0 → norm 1.0 (clamps at max).
    constexpr std::string_view legacyJson = R"({
        "buses": [
            { "name": "Master",
              "effects": [
                { "kind": "saturation", "drive": 4.0, "mix": 0.3 },
                { "kind": "saturation", "drive": 2.5, "mix": 0.2 },
                { "kind": "saturation", "drive": 99.0, "mix": 0.4 }
              ] }
        ]
    })";
    auto r1 = BusConfigLoader::ParseFromJson(legacyJson);
    EXPECT_OK(r1);
    const auto& mLeg = r1.busGraph.buses[kBusMaster];
    EXPECT(mLeg.effectCount == 3);
    EXPECT(std::abs(mLeg.effects[0].saturationDrive - 1.0f)      < 1e-6f); // 4.0 → 1.0
    EXPECT(std::abs(mLeg.effects[1].saturationDrive - 0.5f)      < 1e-6f); // 2.5 → 0.5
    EXPECT(std::abs(mLeg.effects[2].saturationDrive - 1.0f)      < 1e-6f); // 99 clamps to 1.0
    EXPECT(mLeg.effects[0].saturationMode == 0);  // default Tanh
    EXPECT(mLeg.effects[1].saturationMode == 0);
    EXPECT(mLeg.effects[2].saturationMode == 0);
    std::printf("    legacy drive migration: 4.0→1.0, 2.5→0.5, 99.0→clamp(1.0) OK\n");

    // b) Already-normalized drive (≤ 1.0) is unchanged. Each of the
    //    four modes parses correctly.
    constexpr std::string_view normJson = R"({
        "buses": [
            { "name": "Master",
              "effects": [
                { "kind": "saturation", "drive": 0.0,  "mode": "tanh" },
                { "kind": "saturation", "drive": 0.5,  "mode": "tube" },
                { "kind": "saturation", "drive": 0.75, "mode": "tape" },
                { "kind": "saturation", "drive": 1.0,  "mode": "diode" }
              ] }
        ]
    })";
    auto r2 = BusConfigLoader::ParseFromJson(normJson);
    EXPECT_OK(r2);
    const auto& mNorm = r2.busGraph.buses[kBusMaster];
    EXPECT(mNorm.effectCount == 4);
    EXPECT(mNorm.effects[0].saturationDrive == 0.0f);
    EXPECT(mNorm.effects[1].saturationDrive == 0.5f);
    EXPECT(mNorm.effects[2].saturationDrive == 0.75f);
    EXPECT(mNorm.effects[3].saturationDrive == 1.0f);
    EXPECT(mNorm.effects[0].saturationMode == 0);   // tanh
    EXPECT(mNorm.effects[1].saturationMode == 1);   // tube
    EXPECT(mNorm.effects[2].saturationMode == 2);   // tape
    EXPECT(mNorm.effects[3].saturationMode == 3);   // diode
    std::printf("    normalized drive + mode parse: all 4 modes OK\n");

    // c) Unknown mode string is rejected with a clear error.
    constexpr std::string_view badModeJson = R"({
        "buses": [
            { "name": "Master",
              "effects": [
                { "kind": "saturation", "mode": "fuzzbox" }
              ] }
        ]
    })";
    auto r3 = BusConfigLoader::ParseFromJson(badModeJson);
    EXPECT(!r3.ok);
    EXPECT(r3.error.find("fuzzbox") != std::string::npos);
    std::printf("    unknown mode rejected: error='%s'\n", r3.error.c_str());
}

// =============================================================================
// 3b. v0.59.0 Phase 4: saturation tone tilt JSON key.
//
//   a) `"tone"` is accepted as a number and round-trips into
//      EffectConfig.saturationTone.
//   b) Out-of-range values are clamped at load time (-1..+1).
//   c) Absent `tone` key defaults to 0.0 (Phase 4 bypass — existing
//      configs sound identical to pre-v0.59.0).
// =============================================================================
void TestSaturationToneTilt() {
    std::printf("  [v0.59.0: saturation tone tilt JSON key]\n");

    constexpr std::string_view js = R"({
        "buses": [
            { "name": "Master",
              "effects": [
                { "kind": "saturation", "drive": 0.5, "tone":  0.5 },
                { "kind": "saturation", "drive": 0.5, "tone": -1.0 },
                { "kind": "saturation", "drive": 0.5, "tone":  2.0 },
                { "kind": "saturation", "drive": 0.5, "tone": -5.0 },
                { "kind": "saturation", "drive": 0.5             }
              ] }
        ]
    })";
    auto r = BusConfigLoader::ParseFromJson(js);
    EXPECT_OK(r);
    const auto& m = r.busGraph.buses[kBusMaster];
    EXPECT(m.effectCount == 5);
    EXPECT(std::abs(m.effects[0].saturationTone -  0.5f) < 1e-6f);
    EXPECT(std::abs(m.effects[1].saturationTone - -1.0f) < 1e-6f);
    EXPECT(std::abs(m.effects[2].saturationTone -  1.0f) < 1e-6f);   // clamped +
    EXPECT(std::abs(m.effects[3].saturationTone - -1.0f) < 1e-6f);   // clamped -
    EXPECT(m.effects[4].saturationTone == 0.0f);                     // default
    std::printf("    tone parsed: 0.5, -1.0, clamp(+1), clamp(-1), default(0) OK\n");
}

// =============================================================================
// 4. Engine integration: parsed config Initialize()s a real runtime.
// =============================================================================
void TestEndToEndInitialize() {
    std::printf("  [end-to-end: parsed config drives a real AudioRuntime]\n");
    constexpr std::string_view json = R"({
        "buses": [
            { "name": "Master" },
            { "name": "Music",  "parent": "Master", "gain_db": -3.0,
              "effects": [
                { "kind": "compressor",
                  "threshold_db": -30.0, "ratio": 8.0,
                  "attack_ms": 5.0, "release_ms": 250.0,
                  "sidechain_bus": "Sfx" }
              ] },
            { "name": "Sfx", "parent": "Master" }
        ]
    })";
    auto r = BusConfigLoader::ParseFromJson(json);
    EXPECT_OK(r);

    AudioRuntime rt;
    AudioConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    cfg.outputMode = AudioOutputMode::Stereo;
    cfg.busGraph   = r.busGraph;

    AudioRuntimeDependencies deps;
    deps.backend = std::make_unique<NullAudioBackend>();
    auto rc = rt.Initialize(cfg, std::move(deps));
    EXPECT(rc == AudioResult::Success);
    rt.Shutdown();
    std::printf("    OK (engine accepted parsed config)\n");
}

// =============================================================================
// 5. Malformed JSON returns ok=false with line number.
// =============================================================================
void TestMalformedJsonReportsLine() {
    std::printf("  [malformed JSON: error has line number]\n");
    // Syntactically invalid: missing closing brace.
    constexpr std::string_view bad = R"({
        "buses": [
            { "name": "Master"
        ]
    })";
    auto r = BusConfigLoader::ParseFromJson(bad);
    EXPECT(!r.ok);
    EXPECT(!r.error.empty());
    EXPECT(r.errorLine > 0);
    std::printf("    error: '%s' at line %d\n", r.error.c_str(), r.errorLine);
}

// =============================================================================
// 6. Unknown effect kind returns descriptive error.
// =============================================================================
void TestUnknownEffectKind() {
    std::printf("  [unknown effect kind: error names the bad value]\n");
    constexpr std::string_view bad = R"({
        "buses": [
            { "name": "Master",
              "effects": [{ "kind": "magic_distortion" }] }
        ]
    })";
    auto r = BusConfigLoader::ParseFromJson(bad);
    EXPECT(!r.ok);
    EXPECT(r.error.find("magic_distortion") != std::string::npos);
    std::printf("    error: '%s'\n", r.error.c_str());
}

// =============================================================================
// 7. Unresolved sidechain bus reference returns descriptive error.
// =============================================================================
void TestUnresolvedSidechainBus() {
    std::printf("  [unresolved sidechain_bus: error names the missing bus]\n");
    constexpr std::string_view bad = R"({
        "buses": [
            { "name": "Master" },
            { "name": "Music", "parent": "Master",
              "effects": [
                { "kind": "compressor", "threshold_db": -20.0,
                  "sidechain_bus": "DoesNotExist" }
              ] }
        ]
    })";
    auto r = BusConfigLoader::ParseFromJson(bad);
    EXPECT(!r.ok);
    EXPECT(r.error.find("DoesNotExist") != std::string::npos);
    std::printf("    error: '%s'\n", r.error.c_str());
}

// =============================================================================
// 8. Unresolved parent reference also errors.
// =============================================================================
void TestUnresolvedParent() {
    std::printf("  [unresolved parent: error names the missing parent]\n");
    constexpr std::string_view bad = R"({
        "buses": [
            { "name": "Master" },
            { "name": "Music", "parent": "Nope" }
        ]
    })";
    auto r = BusConfigLoader::ParseFromJson(bad);
    EXPECT(!r.ok);
    EXPECT(r.error.find("Nope") != std::string::npos);
    std::printf("    error: '%s'\n", r.error.c_str());
}

// =============================================================================
// 9. Tolerates unknown keys (forward-compat).
// =============================================================================
void TestForwardCompatUnknownKeys() {
    std::printf("  [forward compat: unknown keys ignored, not failed]\n");
    constexpr std::string_view json = R"({
        "version": "v999",
        "buses": [
            { "name": "Master", "future_field": 42,
              "effects": [{ "kind": "gain", "future_param": "test" }] }
        ]
    })";
    auto r = BusConfigLoader::ParseFromJson(json);
    EXPECT_OK(r);
    EXPECT(r.busGraph.buses[kBusMaster].effectCount == 1);
    std::printf("    OK (unknown keys skipped)\n");
}

// =============================================================================
// 9b. v0.80.9: top-level global_reverb_send scalar.
//     Present → optional populated. Absent → nullopt (caller's
//     AudioConfig default 0.0 preserved). Out-of-range → error.
// =============================================================================
void TestGlobalReverbSend() {
    std::printf("  [global_reverb_send: parse, absent, range]\n");

    // (a) Present: parses into the optional.
    {
        constexpr std::string_view json = R"({
            "global_reverb_send": 0.15,
            "buses": [
                { "name": "Master" },
                { "name": "Reverb", "parent": "Master",
                  "effects": [{ "kind": "reverb", "dry_gain_db": -60.0,
                                "wet_gain_db": 0.0 }] }
            ]
        })";
        auto r = BusConfigLoader::ParseFromJson(json);
        EXPECT_OK(r);
        EXPECT(r.globalReverbSend.has_value());
        EXPECT(r.globalReverbSend.value() > 0.149f
                && r.globalReverbSend.value() < 0.151f);
        // The Reverb bus is the first non-Master bus → it lands at
        // id == kBusReverb (1). Buses are indexed by id, so the bus at
        // kBusReverb should carry the single reverb effect.
        EXPECT(r.busGraph.busCount == 2);
        const auto& reverbBus = r.busGraph.buses[kBusReverb];
        EXPECT(reverbBus.effectCount == 1);
        EXPECT(reverbBus.effects[0].kind == EffectKind::Reverb);
        std::printf("    OK (present: 0.15, Reverb bus at id=kBusReverb)\n");
    }

    // (b) Absent: optional stays empty so the C++ default holds.
    {
        constexpr std::string_view json = R"({
            "buses": [ { "name": "Master" } ]
        })";
        auto r = BusConfigLoader::ParseFromJson(json);
        EXPECT_OK(r);
        EXPECT(!r.globalReverbSend.has_value());
        std::printf("    OK (absent: nullopt, default preserved)\n");
    }

    // (c) Out of range: descriptive error, ok=false.
    {
        constexpr std::string_view json = R"({
            "global_reverb_send": 1.5,
            "buses": [ { "name": "Master" } ]
        })";
        auto r = BusConfigLoader::ParseFromJson(json);
        EXPECT(!r.ok);
        EXPECT(r.error.find("global_reverb_send") != std::string::npos);
        std::printf("    OK (out-of-range 1.5 rejected: %s)\n",
                r.error.c_str());
    }
}

// =============================================================================
// 10. Backward-compat: old config without "buses" key still parses,
//     producing an empty bus graph the engine auto-fills with
//     master-only at Initialize.
// =============================================================================
void TestBackwardCompatNoBusesKey() {
    std::printf("  [back-compat: no buses key → empty graph (engine auto-builds master)]\n");
    constexpr std::string_view json = R"({
        "sample_rate": 48000,
        "buffer_size": 512
    })";
    auto r = BusConfigLoader::ParseFromJson(json);
    EXPECT_OK(r);
    EXPECT(r.busGraph.busCount == 0);
    std::printf("    OK (busCount=0; engine auto-builds master)\n");
}

// =============================================================================
// 11. AudioRuntime::FindBusIdByName resolves buses by their debug
//     name. Bridges the gap between code that knows bus names
//     (configs, hosts) and code that needs BusId tokens.
// =============================================================================
void TestFindBusIdByName() {
    std::printf("  [FindBusIdByName: resolves by debugName after Initialize]\n");
    constexpr std::string_view json = R"({
        "buses": [
            { "name": "Master" },
            { "name": "Music",    "parent": "Master" },
            { "name": "LocalSfx", "parent": "Master" },
            { "name": "RemoteSfx","parent": "Master" }
        ]
    })";
    auto pr = BusConfigLoader::ParseFromJson(json);
    EXPECT_OK(pr);

    AudioRuntime rt;
    AudioConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    cfg.outputMode = AudioOutputMode::Stereo;
    cfg.busGraph   = pr.busGraph;
    AudioRuntimeDependencies deps;
    deps.backend = std::make_unique<NullAudioBackend>();
    auto rc = rt.Initialize(cfg, std::move(deps));
    EXPECT(rc == AudioResult::Success);

    // Each name resolves to a valid, distinct BusId.
    BusId master    = rt.FindBusIdByName("Master");
    BusId music     = rt.FindBusIdByName("Music");
    BusId localSfx  = rt.FindBusIdByName("LocalSfx");
    BusId remoteSfx = rt.FindBusIdByName("RemoteSfx");
    EXPECT(master    == kBusMaster);   // Master is always pinned to id 0
    EXPECT(music     != kInvalidBusId);
    EXPECT(localSfx  != kInvalidBusId);
    EXPECT(remoteSfx != kInvalidBusId);
    EXPECT(music     != localSfx);
    EXPECT(localSfx  != remoteSfx);

    // Unknown names return kInvalidBusId.
    EXPECT(rt.FindBusIdByName("DoesNotExist") == kInvalidBusId);
    EXPECT(rt.FindBusIdByName("")             == kInvalidBusId);

    rt.Shutdown();
    std::printf("    OK (4/4 resolved, unknowns return kInvalidBusId)\n");
}

} // namespace

int main() {
    std::printf("[bus_config_loader_test]\n");
    TestMinimalConfig();
    TestMultiTierDuckingShape();
    TestAllEffectKinds();
    TestSaturationModeAndMigration();
    TestSaturationToneTilt();         // v0.59.0 Phase 4
    TestEndToEndInitialize();
    TestMalformedJsonReportsLine();
    TestUnknownEffectKind();
    TestUnresolvedSidechainBus();
    TestUnresolvedParent();
    TestForwardCompatUnknownKeys();
    TestGlobalReverbSend();
    TestBackwardCompatNoBusesKey();
    TestFindBusIdByName();
    std::printf("[bus_config_loader_test] PASSED\n");
    return 0;
}
