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

// tests/unit/global_parameter_test.cpp
//
// Validates the global (RTPC) parameter store backing
// Gool.set_rtpc / Gool.get_rtpc. Covers:
//   - HashParameterName: stable, collision-free for engine names,
//     remaps reserved IDs above HostBase
//   - Set/Get round-trip
//   - Update vs first-set distinction (BudgetExceeded only on new ids)
//   - Clear semantics
//   - Budget enforcement at AudioConfig::maxGlobalParameters

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/types.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <vector>

namespace {

class OfflineBackend final : public audio::IAudioBackend {
public:
    audio::AudioResult Start(const audio::AudioBackendConfig& cfg,
                              audio::IAudioRenderCallback*    cb) override {
        cfg_ = cfg; cb_ = cb;
        return audio::AudioResult::Success;
    }
    void     Stop() override { cb_ = nullptr; }
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "OfflineGlobalParam"; }

private:
    audio::AudioBackendConfig    cfg_{};
    audio::IAudioRenderCallback* cb_ = nullptr;
};

void InitRuntime(audio::AudioRuntime& rt, uint32_t maxParams = 256) {
    audio::AudioConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    cfg.outputMode = audio::AudioOutputMode::Stereo;
    cfg.maxGlobalParameters = maxParams;

    audio::AudioRuntimeDependencies deps;
    deps.backend = std::make_unique<OfflineBackend>();

    auto rc = rt.Initialize(cfg, std::move(deps));
    assert(rc == audio::AudioResult::Success);
}

void TestHashIsStable() {
    std::cout << "  [HashParameterName is constexpr-stable and collision-aware]\n";
    constexpr auto h1 = audio::HashParameterName("health");
    constexpr auto h2 = audio::HashParameterName("health");
    static_assert(h1 == h2);

    constexpr auto h3 = audio::HashParameterName("fatigue");
    static_assert(h1 != h3);

    // Hashes that would otherwise land in the engine-reserved range
    // [1, HostBase) get bumped above HostBase. Verify by hashing many
    // names and asserting none collide with the reserved IDs.
    const char* names[] = {
        "health", "fatigue", "wetness", "temperature", "speed",
        "rpm", "throttle", "altitude", "wind", "fuel",
    };
    for (auto* n : names) {
        const auto h = audio::HashParameterName(n);
        assert(h >= audio::AudioParameterIds::HostBase);
        assert(h != audio::kInvalidParameterId);
    }
}

void TestSetGetRoundTrip() {
    std::cout << "  [Set + Get returns the stored value]\n";
    audio::AudioRuntime rt; InitRuntime(rt);

    const auto p = audio::HashParameterName("health");
    auto rc = rt.SetGlobalParameter(p, 0.75f);
    assert(rc == audio::AudioResult::Success);

    float v = -1.0f;
    bool found = rt.GetGlobalParameter(p, v);
    assert(found);
    assert(v == 0.75f);

    // Update is allowed, never exceeds the budget.
    rc = rt.SetGlobalParameter(p, 0.10f);
    assert(rc == audio::AudioResult::Success);
    rt.GetGlobalParameter(p, v);
    assert(v == 0.10f);

    rt.Shutdown();
}

void TestUnsetReturnsFalse() {
    std::cout << "  [Get on never-set parameter returns false]\n";
    audio::AudioRuntime rt; InitRuntime(rt);

    const auto p = audio::HashParameterName("never_set_this");
    float v = 12345.0f;
    bool found = rt.GetGlobalParameter(p, v);
    assert(!found);
    assert(v == 12345.0f);  // outValue untouched on miss

    rt.Shutdown();
}

void TestClearSemantics() {
    std::cout << "  [Clear removes the entry; subsequent Get is false]\n";
    audio::AudioRuntime rt; InitRuntime(rt);

    const auto p = audio::HashParameterName("temp");
    rt.SetGlobalParameter(p, 1.5f);
    assert(rt.GetGlobalParameterCount() == 1u);

    bool removed = rt.ClearGlobalParameter(p);
    assert(removed);
    assert(rt.GetGlobalParameterCount() == 0u);

    float v = 0.0f;
    assert(!rt.GetGlobalParameter(p, v));

    // Clearing again is a no-op (returns false).
    bool removedAgain = rt.ClearGlobalParameter(p);
    assert(!removedAgain);

    rt.Shutdown();
}

void TestBudgetEnforcement() {
    std::cout << "  [Budget exceeded only on NEW ids; updates always allowed]\n";
    audio::AudioRuntime rt; InitRuntime(rt, /*maxParams*/ 4);

    // Fill the budget.
    char buf[32];
    for (int i = 0; i < 4; ++i) {
        std::snprintf(buf, sizeof(buf), "param_%d", i);
        auto rc = rt.SetGlobalParameter(audio::HashParameterName(buf),
                                         static_cast<float>(i));
        assert(rc == audio::AudioResult::Success);
    }
    assert(rt.GetGlobalParameterCount() == 4u);

    // Updating an existing parameter still works (no new id needed).
    auto rcUpdate = rt.SetGlobalParameter(
        audio::HashParameterName("param_0"), 99.0f);
    assert(rcUpdate == audio::AudioResult::Success);
    float v = 0.0f;
    rt.GetGlobalParameter(audio::HashParameterName("param_0"), v);
    assert(v == 99.0f);

    // A NEW id over the budget is rejected.
    auto rcNew = rt.SetGlobalParameter(
        audio::HashParameterName("over_budget"), 1.0f);
    assert(rcNew == audio::AudioResult::BudgetExceeded);
    assert(rt.GetGlobalParameterCount() == 4u);

    // After clearing one, a new id fits again.
    rt.ClearGlobalParameter(audio::HashParameterName("param_3"));
    auto rcAfterClear = rt.SetGlobalParameter(
        audio::HashParameterName("over_budget"), 1.0f);
    assert(rcAfterClear == audio::AudioResult::Success);
    assert(rt.GetGlobalParameterCount() == 4u);

    rt.Shutdown();
}

void TestNotInitialized() {
    std::cout << "  [Set before Initialize returns NotInitialized]\n";
    audio::AudioRuntime rt;
    auto rc = rt.SetGlobalParameter(
        audio::HashParameterName("health"), 0.5f);
    assert(rc == audio::AudioResult::NotInitialized);
}

void TestInvalidIdRejected() {
    std::cout << "  [Set with kInvalidParameterId returns InvalidArgument]\n";
    audio::AudioRuntime rt; InitRuntime(rt);
    auto rc = rt.SetGlobalParameter(audio::kInvalidParameterId, 0.5f);
    assert(rc == audio::AudioResult::InvalidArgument);
    rt.Shutdown();
}

} // namespace

int main() {
    std::cout << "[global_parameter_test]\n";
    TestHashIsStable();
    TestSetGetRoundTrip();
    TestUnsetReturnsFalse();
    TestClearSemantics();
    TestBudgetEnforcement();
    TestNotInitialized();
    TestInvalidIdRejected();
    std::cout << "[global_parameter_test] PASSED\n";
    return 0;
}
