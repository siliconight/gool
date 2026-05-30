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

// tests/unit/air_absorption_test.cpp
//
// Verifies that distance-driven air absorption progressively damps highs.
// Runs the full runtime through the offline backend so the chain
// SpatialEmitterView.distance → spatializer.lowPassAmount → mixer LPF is
// exercised. A 10 kHz sine source is rendered at three distances: 1 m,
// 100 m, and 250 m. With air absorption on (default), peak amplitude
// monotonically decreases with distance because the per-voice LPF cuts
// more of the 10 kHz content as the source recedes. With air absorption
// off, the only attenuation is volume-floor distance falloff, and the
// 10 kHz tone passes through unaffected by the LPF.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/bus.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/types.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kBufferSize = 256;
constexpr float    kPi = 3.14159265358979323846f;

std::vector<float> SineMono(uint32_t frames, float hz) {
    std::vector<float> v(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        v[i] = std::sin(2.0f * kPi * hz * static_cast<float>(i)
                                / static_cast<float>(kSampleRate));
    }
    return v;
}

float Peak(const float* d, size_t n) {
    float p = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float a = std::fabs(d[i]);
        if (a > p) p = a;
    }
    return p;
}

class OfflineBackend final : public IAudioBackend {
public:
    AudioResult Start(const AudioBackendConfig& cfg, IAudioRenderCallback* cb) override {
        cfg_ = cfg; cb_ = cb;
        scratch_.assign(static_cast<size_t>(cfg.bufferSize) * cfg.channels, 0.0f);
        return AudioResult::Success;
    }
    void Stop() override { cb_ = nullptr; }
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "Offline"; }
    void Render(uint32_t frames, std::vector<float>& out) {
        out.clear();
        if (!cb_) return;
        const uint32_t bs = cfg_.bufferSize, ch = cfg_.channels;
        out.reserve(static_cast<size_t>(frames + bs) * ch);
        uint32_t produced = 0;
        while (produced < frames) {
            cb_->OnRender(scratch_.data(), bs, ch);
            const uint32_t take = std::min(bs, frames - produced);
            out.insert(out.end(),
                       scratch_.begin(),
                       scratch_.begin() + take * ch);
            produced += take;
        }
    }
private:
    AudioBackendConfig cfg_{};
    IAudioRenderCallback* cb_ = nullptr;
    std::vector<float> scratch_;
};

float MeasurePeakAtDistance(float distance, bool airOn) {
    AudioConfig cfg;
    cfg.sampleRate                 = kSampleRate;
    cfg.bufferSize                 = kBufferSize;
    cfg.outputMode                 = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters   = 4;
    cfg.budget.maxRegisteredSounds = 4;
    cfg.budget.maxStreamingAssets  = 1;
    cfg.budget.maxStreamingVoices  = 1;
    cfg.budget.maxVoiceSources     = 0;
    cfg.enableOcclusion            = false;     // isolate to air absorption
    cfg.enableAirAbsorption        = airOn;
    // Use the default airAbsorptionPerMeter = 1/250.

    AudioRuntime rt;
    auto backend = std::make_unique<OfflineBackend>();
    OfflineBackend* bp = backend.get();

    AudioRuntimeDependencies deps;
    deps.backend = std::move(backend);
    if (rt.Initialize(cfg, std::move(deps)) != AudioResult::Success) {
        ++gFails; return 0.0f;
    }

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    rt.SetListener(lis);

    constexpr AudioSoundId kSnd = 1;
    auto pcm = SineMono(kSampleRate, 10000.0f);    // 10 kHz, 1 s
    rt.RegisterPcmSound(kSnd, pcm, kSampleRate, 1);

    SoundDefinition def;
    def.soundId               = kSnd;
    def.category              = AudioCategory::SFX;
    def.targetBus             = kBusMaster;
    def.spatialized           = true;
    def.looping               = true;
    def.occlusionEnabled      = false;
    // Generous attenuation so distance gain doesn't dominate; we want the
    // LPF effect to be the visible variable, not volume falloff.
    def.attenuation.minDistance = 1.0f;
    def.attenuation.maxDistance = 500.0f;
    def.attenuation.volumeFloor = 0.5f;
    rt.RegisterSoundDefinition(def);

    EmitterDescriptor ed;
    ed.soundId          = kSnd;
    ed.position         = {distance, 0.0f, 0.0f};
    ed.targetBus        = kBusMaster;
    ed.category         = AudioCategory::SFX;
    ed.isLooping        = true;
    ed.isSpatialized    = true;
    ed.occlusionEnabled = false;
    ed.priority         = AudioPriority::Normal;
    ed.attenuation      = def.attenuation;
    auto h = rt.CreateEmitter(ed);
    if (!h) { ++gFails; return 0.0f; }

    // Burn 20 ticks for spatializer + filter to stabilise.
    std::vector<float> out;
    for (int i = 0; i < 20; ++i) {
        rt.Update(0.025f);
        bp->Render(kSampleRate / 40, out);
    }
    // Measure peak over 250 ms.
    std::vector<float> meas;
    for (int i = 0; i < 10; ++i) {
        rt.Update(0.025f);
        bp->Render(kSampleRate / 40, out);
        meas.insert(meas.end(), out.begin(), out.end());
    }

    rt.DestroyEmitter(h.value());
    rt.Shutdown();
    return Peak(meas.data(), meas.size());
}

void TestAirAbsorptionMonotonicWithDistance() {
    const float p1   = MeasurePeakAtDistance(  1.0f, /*airOn*/ true);
    const float p100 = MeasurePeakAtDistance(100.0f, /*airOn*/ true);
    const float p250 = MeasurePeakAtDistance(250.0f, /*airOn*/ true);
    std::printf("  10 kHz sine, air on: 1m=%.3f  100m=%.3f  250m=%.4f\n", p1, p100, p250);
    // 1 m: airAmount = 0.004; basically bypass; peak should be near full.
    EXPECT(p1 > 0.4f);
    // 100 m: airAmount = 0.4; substantial damping.
    EXPECT(p100 < p1 * 0.5f);
    // 250 m: airAmount = 1.0 → cutoff ≈ 500 Hz, 10 kHz hit by ~25+ dB rolloff.
    EXPECT(p250 < p100 * 0.5f);
}

void TestAirAbsorptionDisabledLeavesHighsAlone() {
    const float p1   = MeasurePeakAtDistance(  1.0f, /*airOn*/ false);
    const float p250 = MeasurePeakAtDistance(250.0f, /*airOn*/ false);
    std::printf("  10 kHz sine, air off: 1m=%.3f  250m=%.3f\n", p1, p250);
    // Both should be in the same ballpark; only volume-floor falloff
    // separates them, no LPF damping. The volumeFloor=0.5 in attenuation
    // settings keeps 250 m at ≥ 50% of 1 m.
    EXPECT(p250 > p1 * 0.45f);
}

} // namespace

int main() {
    std::printf("[air_absorption_test] running...\n");
    TestAirAbsorptionMonotonicWithDistance();
    TestAirAbsorptionDisabledLeavesHighsAlone();
    if (gFails == 0) {
        std::printf("[air_absorption_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[air_absorption_test] %d failure(s)\n", gFails);
    return 1;
}
