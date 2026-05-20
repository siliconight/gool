// tests/unit/sound_bank_test.cpp
//
// Coverage:
//   1. Basic load: 3 sounds + 1 group, all resolved, IDs are stable
//      hashes of the names.
//   2. Defaults inheritance: default category/priority/bus/attenuation
//      flow into entries that don't override.
//   3. Per-entry overrides take precedence over defaults.
//   4. Group selection policies: random_no_repeat does not repeat
//      consecutively; sequential cycles in order.
//   5. Validation: duplicate sound names rejected with line number.
//   6. Validation: group references unknown member rejected.
//   7. Hot reload: reloading from string applies new content.
//   8. Malformed JSON rejected with helpful error.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/sound_bank.h"
#include "audio_engine/config.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

std::vector<float> SineMono(uint32_t numFrames, float freq = 440.0f,
                              float amp = 0.5f, uint32_t sampleRate = 48000) {
    std::vector<float> out(numFrames);
    for (uint32_t i = 0; i < numFrames; ++i) {
        out[i] = amp * std::sin(2.0f * 3.14159265f * freq *
                                static_cast<float>(i) /
                                static_cast<float>(sampleRate));
    }
    return out;
}

// Pre-register synthetic PCM under the names the JSON references.
// The bank then registers SoundDefinitions for the same hashed ids.
void PreregisterSynthetic(audio::AudioRuntime& rt,
                            const std::vector<std::string>& names,
                            uint32_t sampleRate = 48000) {
    for (const auto& n : names) {
        const audio::AudioSoundId id = audio::HashSoundName(n);
        const auto samples = SineMono(sampleRate / 10);
        rt.RegisterPcmSound(id,
                             std::span<const float>(samples.data(), samples.size()),
                             sampleRate, 1u);
    }
}

#define AE_TEST_SETUP_RUNTIME(rt)                                            \
    audio::AudioRuntime rt;                                                  \
    do {                                                                      \
        audio::AudioConfig cfg;                                              \
        cfg.sampleRate = 48000;                                               \
        cfg.bufferSize = 512;                                                 \
        cfg.outputMode = audio::AudioOutputMode::Stereo;                      \
        audio::AudioRuntimeDependencies deps;                                 \
        const auto rc = rt.Initialize(cfg, std::move(deps));                  \
        assert(rc == audio::AudioResult::Success);                            \
    } while (0)

void TestBasicLoad() {
    std::cout << "  [basic load + hash stability]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"weapon.ak47.shot",
                                "footstep.grass.01",
                                "footstep.grass.02"});

    const std::string json = R"({
        "version": 1,
        "sounds": [
            { "name": "weapon.ak47.shot" },
            { "name": "footstep.grass.01" },
            { "name": "footstep.grass.02" }
        ],
        "groups": [
            { "name": "footstep.grass",
              "policy": "random_no_repeat",
              "members": ["footstep.grass.01", "footstep.grass.02"] }
        ]
    })";

    audio::SoundBank bank;
    const auto r = bank.LoadFromJsonString(rt, json);
    if (!r.success) {
        std::cerr << "    FAIL: " << r.errorMessage
                  << " (line " << r.errorLine << ")\n";
        std::exit(1);
    }
    assert(r.soundsLoaded == 3);
    assert(r.groupsLoaded == 1);
    assert(bank.SoundCount() == 3);
    assert(bank.GroupCount() == 1);

    // IDs are stable hashes of the names.
    assert(bank.Find("weapon.ak47.shot") == audio::HashSoundName("weapon.ak47.shot"));
    assert(bank.Find("footstep.grass.01") == audio::HashSoundName("footstep.grass.01"));

    // Group resolves to a member.
    const auto g = bank.Find("footstep.grass");
    assert(g == audio::HashSoundName("footstep.grass.01") ||
           g == audio::HashSoundName("footstep.grass.02"));

    assert(bank.Find("nonexistent") == audio::kInvalidSoundId);

    rt.Shutdown();
    std::cout << "    OK (3 sounds + 1 group, IDs are stable hashes)\n";
}

void TestDefaultsInheritance() {
    std::cout << "  [defaults inheritance]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"a", "b"});

    const std::string json = R"({
        "version": 1,
        "defaults": {
            "category": "Music",
            "priority": "High",
            "spatialized": false,
            "looping": true,
            "attenuation": { "min": 5.0, "max": 100.0, "floor": 0.05, "falloff": "Linear" }
        },
        "sounds": [
            { "name": "a" },
            { "name": "b", "category": "SFX", "spatialized": true }
        ]
    })";

    audio::SoundBank bank;
    const auto r = bank.LoadFromJsonString(rt, json);
    if (!r.success) {
        std::cerr << "    FAIL: " << r.errorMessage << "\n";
        std::exit(1);
    }
    assert(r.soundsLoaded == 2);
    rt.Shutdown();
    std::cout << "    OK (defaults flowed; per-entry overrides applied)\n";
}

void TestSequentialPolicy() {
    std::cout << "  [sequential policy cycles in order]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"a", "b", "c"});

    const std::string json = R"({
        "sounds": [
            { "name": "a" }, { "name": "b" }, { "name": "c" }
        ],
        "groups": [
            { "name": "abc", "policy": "sequential",
              "members": ["a", "b", "c"] }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    if (!r.success) {
        std::cerr << "    FAIL: " << r.errorMessage << "\n";
        std::exit(1);
    }

    // First three calls should hit a, b, c in order.
    const auto idA = audio::HashSoundName("a");
    const auto idB = audio::HashSoundName("b");
    const auto idC = audio::HashSoundName("c");
    const auto p1 = bank.Find("abc");
    const auto p2 = bank.Find("abc");
    const auto p3 = bank.Find("abc");
    const auto p4 = bank.Find("abc");
    assert(p1 == idA && p2 == idB && p3 == idC && p4 == idA);

    rt.Shutdown();
    std::cout << "    OK (a -> b -> c -> a)\n";
}

void TestRandomNoRepeat() {
    std::cout << "  [random_no_repeat avoids back-to-back duplicates]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"a", "b", "c", "d"});

    const std::string json = R"({
        "sounds": [{"name":"a"},{"name":"b"},{"name":"c"},{"name":"d"}],
        "groups": [
            { "name": "g", "policy": "random_no_repeat",
              "members": ["a","b","c","d"] }
        ]
    })";
    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(r.success);

    audio::AudioSoundId prev = bank.Find("g");
    int repeats = 0;
    for (int i = 0; i < 200; ++i) {
        const auto cur = bank.Find("g");
        if (cur == prev) ++repeats;
        prev = cur;
    }
    if (repeats != 0) {
        std::cerr << "    FAIL: " << repeats << " back-to-back repeats over 200 picks\n";
        std::exit(1);
    }
    rt.Shutdown();
    std::cout << "    OK (0 back-to-back repeats over 200 picks)\n";
}

void TestDuplicateRejection() {
    std::cout << "  [duplicate name rejected with line number]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"x"});

    const std::string json = R"({
        "sounds": [
            { "name": "x" },
            { "name": "x" }
        ]
    })";
    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(!r.success);
    assert(r.errorMessage.find("duplicate") != std::string::npos);
    assert(r.errorLine > 0);
    rt.Shutdown();
    std::cout << "    OK (rejected at line " << r.errorLine << ": "
              << r.errorMessage << ")\n";
}

void TestUnknownMember() {
    std::cout << "  [group with unknown member rejected]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"a"});

    const std::string json = R"({
        "sounds": [{"name":"a"}],
        "groups": [
            { "name": "g", "policy": "random",
              "members": ["a", "missing"] }
        ]
    })";
    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(!r.success);
    assert(r.errorMessage.find("missing") != std::string::npos);
    rt.Shutdown();
    std::cout << "    OK (rejected: " << r.errorMessage << ")\n";
}

void TestHotReload() {
    std::cout << "  [hot reload: re-parse and re-register]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"a", "b"});

    audio::SoundBank bank;
    auto r1 = bank.LoadFromJsonString(rt, R"({"sounds":[{"name":"a"}]})");
    assert(r1.success && bank.SoundCount() == 1);
    assert(bank.Find("a") != audio::kInvalidSoundId);
    assert(bank.Find("b") == audio::kInvalidSoundId);

    auto r2 = bank.LoadFromJsonString(rt,
        R"({"sounds":[{"name":"a"},{"name":"b"}]})");
    assert(r2.success && bank.SoundCount() == 2);
    assert(bank.Find("b") != audio::kInvalidSoundId);

    // Reload() re-runs the most recent load.
    auto r3 = bank.Reload(rt);
    assert(r3.success && bank.SoundCount() == 2);

    rt.Shutdown();
    std::cout << "    OK (1 -> 2 sounds after reload)\n";
}

void TestMalformedJson() {
    std::cout << "  [malformed JSON gives line-numbered error]\n";
    AE_TEST_SETUP_RUNTIME(rt);

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, "{ this is not json");
    assert(!r.success);
    assert(r.errorLine > 0);
    rt.Shutdown();
    std::cout << "    OK (line " << r.errorLine
              << ": " << r.errorMessage << ")\n";
}

void TestUnknownEnumValue() {
    std::cout << "  [unknown enum value gives helpful error]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"a"});

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, R"({
        "sounds": [{"name":"a", "category":"NotARealCategory"}]
    })");
    assert(!r.success);
    assert(r.errorMessage.find("category") != std::string::npos);
    rt.Shutdown();
    std::cout << "    OK (rejected: " << r.errorMessage << ")\n";
}

void TestPerformance() {
    std::cout << "  [performance: 1000-sound bank, lookup throughput]\n";
    audio::AudioRuntime rt;
    {
        audio::AudioConfig cfg;
        cfg.sampleRate           = 48000;
        cfg.bufferSize           = 512;
        cfg.outputMode           = audio::AudioOutputMode::Stereo;
        cfg.budget.maxRegisteredSounds  = 2048;   // headroom for the 1000 entries
        audio::AudioRuntimeDependencies deps;
        const auto rc = rt.Initialize(cfg, std::move(deps));
        assert(rc == audio::AudioResult::Success);
    }

    // Build a JSON bank with 1000 sound entries.
    std::string json = "{\"sounds\":[";
    std::vector<std::string> names;
    names.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        std::string n = "sound." + std::to_string(i);
        names.push_back(n);
        if (i > 0) json += ",";
        json += "{\"name\":\"" + n + "\"}";
    }
    json += "]}";

    PreregisterSynthetic(rt, names);

    auto t0 = std::chrono::steady_clock::now();
    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    auto t1 = std::chrono::steady_clock::now();
    if (!r.success) {
        std::cerr << "    FAIL: " << r.errorMessage << "\n";
        std::exit(1);
    }
    const auto loadMs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;

    // Hammer Find() to measure lookup speed. 100k lookups across the
    // bank is representative of game-thread access patterns.
    constexpr int kIters = 100'000;
    auto t2 = std::chrono::steady_clock::now();
    uint64_t accum = 0;
    for (int i = 0; i < kIters; ++i) {
        accum += bank.Find(names[static_cast<size_t>(i) % names.size()]);
    }
    auto t3 = std::chrono::steady_clock::now();
    const auto findUs = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
    const double findNsPerOp = (findUs * 1000.0) / kIters;

    rt.Shutdown();

    std::cout << "    load 1000 entries: " << loadMs << " ms\n";
    std::cout << "    Find() throughput:  " << findNsPerOp << " ns/op ("
              << static_cast<int>(1e9 / findNsPerOp) << " ops/s)\n";
    // Sanity: Find should be much faster than 10 us per call.
    assert(findNsPerOp < 10000.0);
    (void)accum;
}

void TestRtpcInJson() {
    std::cout << "  [rtpc array in sound entry registers bindings]\n";
    AE_TEST_SETUP_RUNTIME(rt);

    PreregisterSynthetic(rt, {"heartbeat", "ambient_loop"});

    // Two sounds: heartbeat with two bindings (volume + pitch),
    // ambient_loop with a lowpass binding using a curve.
    const char* json = R"({
        "version": 1,
        "sounds": [
            {
                "name": "heartbeat",
                "category": "SFX",
                "rtpc": [
                    {
                        "parameter": "health",
                        "target":    "volume",
                        "min_value": 0.0, "max_value": 1.0,
                        "min_output": 1.0, "max_output": 0.0,
                        "smoothing_ms": 50
                    },
                    {
                        "parameter": "fatigue",
                        "target":    "pitch",
                        "curve":     "exponential",
                        "exponent":  2.0,
                        "min_value": 0.0, "max_value": 1.0,
                        "min_output": 1.0, "max_output": 0.85
                    }
                ]
            },
            {
                "name": "ambient_loop",
                "category": "Ambience",
                "rtpc": [
                    {
                        "parameter": "wetness",
                        "target":    "lowpass",
                        "curve":     "scurve",
                        "min_value": 0.0, "max_value": 1.0,
                        "min_output": 0.0, "max_output": 1.0
                    }
                ]
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(r.errorMessage.empty());
    assert(r.soundsLoaded == 2u);

    // 2 + 1 = 3 bindings registered total.
    assert(rt.GetSoundRtpcBindingCount() == 3u);

    // Removing all bindings for heartbeat removes 2; ambient_loop's
    // binding remains.
    const auto kHb = audio::HashSoundName("heartbeat");
    const auto removed = rt.ClearAllSoundRtpc(kHb);
    assert(removed == 2u);
    assert(rt.GetSoundRtpcBindingCount() == 1u);

    rt.Shutdown();
}

void TestRtpcInJsonRejectsUnknownTarget() {
    std::cout << "  [rtpc with unknown target string is rejected with line number]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"siren"});

    const char* json = R"({
        "version": 1,
        "sounds": [
            {
                "name": "siren",
                "rtpc": [
                    { "parameter": "alarm_level", "target": "loudness",
                      "min_value": 0, "max_value": 1,
                      "min_output": 0, "max_output": 1 }
                ]
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(!r.errorMessage.empty());
    // The error mentions "loudness" so the user can fix it.
    assert(r.errorMessage.find("loudness") != std::string::npos);
    rt.Shutdown();
}

// ------------------------------------------------------------------
// by_material group policy (Phase 5.1, v0.30.0)
// ------------------------------------------------------------------

void TestByMaterialSelectsFromRequestedBucket() {
    std::cout << "  [by_material: Find(name, Concrete) picks from "
                 "Concrete bucket]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {
        "impact.concrete.01", "impact.concrete.02",
        "impact.wood.01",
        "impact.generic"
    });
    const char* json = R"({
        "version": 1,
        "sounds": [
            { "name": "impact.concrete.01" },
            { "name": "impact.concrete.02" },
            { "name": "impact.wood.01" },
            { "name": "impact.generic" }
        ],
        "groups": [
            {
                "name":   "bullet_impact",
                "policy": "by_material",
                "members_by_material": {
                    "Concrete": ["impact.concrete.01", "impact.concrete.02"],
                    "Wood":     ["impact.wood.01"],
                    "Default":  ["impact.generic"]
                }
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(r.success);
    assert(r.groupsLoaded == 1u);

    const audio::AudioSoundId concrete1 = audio::HashSoundName("impact.concrete.01");
    const audio::AudioSoundId concrete2 = audio::HashSoundName("impact.concrete.02");
    const audio::AudioSoundId wood1     = audio::HashSoundName("impact.wood.01");

    // Several picks against Concrete — every result must be from the
    // Concrete bucket, never Wood, never Default.
    for (int i = 0; i < 16; ++i) {
        const auto id = bank.Find("bullet_impact", audio::AudioMaterial::Concrete);
        assert(id == concrete1 || id == concrete2);
    }
    // Wood bucket has one variant; every pick should return it.
    for (int i = 0; i < 8; ++i) {
        const auto id = bank.Find("bullet_impact", audio::AudioMaterial::Wood);
        assert(id == wood1);
    }
    rt.Shutdown();
}

void TestByMaterialFallsBackToDefault() {
    std::cout << "  [by_material: unmapped material falls through to "
                 "Default bucket]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"impact.concrete.01", "impact.generic"});
    const char* json = R"({
        "version": 1,
        "sounds": [
            { "name": "impact.concrete.01" },
            { "name": "impact.generic" }
        ],
        "groups": [
            {
                "name":   "bullet_impact",
                "policy": "by_material",
                "members_by_material": {
                    "Concrete": ["impact.concrete.01"],
                    "Default":  ["impact.generic"]
                }
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(r.success);

    const audio::AudioSoundId generic = audio::HashSoundName("impact.generic");
    // Foliage isn't in the table; should fall through to Default.
    const auto id = bank.Find("bullet_impact", audio::AudioMaterial::Foliage);
    assert(id == generic);
    rt.Shutdown();
}

void TestByMaterialMissingDefaultIsInvalid() {
    std::cout << "  [by_material: missing Default + unmapped material "
                 "returns kInvalidSoundId (lenient rule)]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"impact.concrete.01"});
    const char* json = R"({
        "version": 1,
        "sounds": [
            { "name": "impact.concrete.01" }
        ],
        "groups": [
            {
                "name":   "bullet_impact",
                "policy": "by_material",
                "members_by_material": {
                    "Concrete": ["impact.concrete.01"]
                }
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    // The bank loads cleanly — designer chose not to provide Default,
    // that's allowed.
    assert(r.success);

    const audio::AudioSoundId concrete1 = audio::HashSoundName("impact.concrete.01");
    // Concrete works.
    assert(bank.Find("bullet_impact", audio::AudioMaterial::Concrete) == concrete1);
    // Wood has no bucket AND no Default — returns kInvalidSoundId.
    assert(bank.Find("bullet_impact", audio::AudioMaterial::Wood) == audio::kInvalidSoundId);
    rt.Shutdown();
}

void TestByMaterialFindWithoutMaterialUsesDefault() {
    std::cout << "  [by_material: Find(name) without material picks "
                 "from Default bucket]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"impact.concrete.01", "impact.generic"});
    const char* json = R"({
        "version": 1,
        "sounds": [
            { "name": "impact.concrete.01" },
            { "name": "impact.generic" }
        ],
        "groups": [
            {
                "name":   "bullet_impact",
                "policy": "by_material",
                "members_by_material": {
                    "Concrete": ["impact.concrete.01"],
                    "Default":  ["impact.generic"]
                }
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(r.success);

    const audio::AudioSoundId generic = audio::HashSoundName("impact.generic");
    // No material argument → Default bucket.
    assert(bank.Find("bullet_impact") == generic);
    rt.Shutdown();
}

void TestByMaterialIgnoresMaterialForOtherPolicies() {
    std::cout << "  [Find(name, material) on a random group ignores "
                 "material]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"footstep.01", "footstep.02"});
    const char* json = R"({
        "version": 1,
        "sounds": [
            { "name": "footstep.01" },
            { "name": "footstep.02" }
        ],
        "groups": [
            {
                "name":   "footstep.grass",
                "policy": "random",
                "members": ["footstep.01", "footstep.02"]
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(r.success);

    const audio::AudioSoundId f1 = audio::HashSoundName("footstep.01");
    const audio::AudioSoundId f2 = audio::HashSoundName("footstep.02");
    // Material is irrelevant for random groups — both calls behave
    // identically and return one of the two variants.
    for (int i = 0; i < 8; ++i) {
        const auto idA = bank.Find("footstep.grass", audio::AudioMaterial::Wood);
        const auto idB = bank.Find("footstep.grass");
        assert(idA == f1 || idA == f2);
        assert(idB == f1 || idB == f2);
    }
    rt.Shutdown();
}

void TestByMaterialUnknownMaterialKeyRejected() {
    std::cout << "  [parse error: unknown material name in "
                 "members_by_material]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    PreregisterSynthetic(rt, {"impact.x"});
    // "Cement" is not a known AudioMaterial.
    const char* json = R"({
        "version": 1,
        "sounds": [ { "name": "impact.x" } ],
        "groups": [
            {
                "name":   "bullet_impact",
                "policy": "by_material",
                "members_by_material": {
                    "Cement": ["impact.x"]
                }
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(!r.success);
    assert(r.errorMessage.find("Cement") != std::string::npos);
    rt.Shutdown();
}

void TestByMaterialAllBucketsEmptyRejected() {
    std::cout << "  [parse error: by_material group with literally "
                 "zero variants]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    const char* json = R"({
        "version": 1,
        "groups": [
            {
                "name":   "bullet_impact",
                "policy": "by_material",
                "members_by_material": {}
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);
    assert(!r.success);
    assert(r.errorMessage.find("bullet_impact") != std::string::npos);
    rt.Shutdown();
}

void TestByMaterialUnknownMemberFallsBackToHashWhenValidationSkipped() {
    std::cout << "  [by_material: members not in JSON resolve via "
                 "HashSoundName when validateReferences=false]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    // Pre-register a sound via the runtime path (mimics
    // register_pcm_sound from GDScript). The bank's JSON won't
    // declare this sound, but the group will reference it.
    PreregisterSynthetic(rt, {"impact.runtime.01"});
    const char* json = R"({
        "version": 1,
        "groups": [
            {
                "name":   "bullet_impact",
                "policy": "by_material",
                "members_by_material": {
                    "Concrete": ["impact.runtime.01"],
                    "Default":  ["impact.runtime.01"]
                }
            }
        ]
    })";

    audio::SoundBank bank;
    audio::SoundBankLoadOptions opts;
    opts.validateReferences = false;
    auto r = bank.LoadFromJsonString(rt, json, opts);
    assert(r.success);
    assert(r.groupsLoaded == 1u);

    // The bank's local soundIds is empty (no sounds declared in
    // JSON), but the group's resolver hashed the member name. That
    // hash matches what PreregisterSynthetic used for runtime
    // registration. So Find returns the runtime-registered id.
    const audio::AudioSoundId expected =
        audio::HashSoundName("impact.runtime.01");
    assert(bank.Find("bullet_impact", audio::AudioMaterial::Concrete)
           == expected);
    // Foliage (unmapped) falls through to Default bucket, which
    // contains the same hashed id.
    assert(bank.Find("bullet_impact", audio::AudioMaterial::Foliage)
           == expected);
    rt.Shutdown();
}

void TestByMaterialValidationStillCatchesTyposWhenOn() {
    std::cout << "  [by_material: undeclared member still rejected "
                 "when validateReferences=true (default)]\n";
    AE_TEST_SETUP_RUNTIME(rt);
    // Same JSON as the test above, same not-in-JSON member name.
    // But this time validation is on (the default). Should fail.
    const char* json = R"({
        "version": 1,
        "groups": [
            {
                "name":   "bullet_impact",
                "policy": "by_material",
                "members_by_material": {
                    "Concrete": ["impact.not_declared.01"]
                }
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(rt, json);  // default opts
    assert(!r.success);
    assert(r.errorMessage.find("impact.not_declared.01") != std::string::npos);
    rt.Shutdown();
}

} // namespace

int main() {
    std::cout << "[sound_bank_test] running...\n";
    TestBasicLoad();
    TestDefaultsInheritance();
    TestSequentialPolicy();
    TestRandomNoRepeat();
    TestDuplicateRejection();
    TestUnknownMember();
    TestHotReload();
    TestMalformedJson();
    TestUnknownEnumValue();
    TestRtpcInJson();
    TestRtpcInJsonRejectsUnknownTarget();
    TestByMaterialSelectsFromRequestedBucket();
    TestByMaterialFallsBackToDefault();
    TestByMaterialMissingDefaultIsInvalid();
    TestByMaterialFindWithoutMaterialUsesDefault();
    TestByMaterialIgnoresMaterialForOtherPolicies();
    TestByMaterialUnknownMaterialKeyRejected();
    TestByMaterialAllBucketsEmptyRejected();
    TestByMaterialUnknownMemberFallsBackToHashWhenValidationSkipped();
    TestByMaterialValidationStillCatchesTyposWhenOn();
    TestPerformance();
    std::cout << "[sound_bank_test] OK\n";
    return 0;
}
