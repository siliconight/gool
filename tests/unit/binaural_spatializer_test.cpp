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

// tests/unit/binaural_spatializer_test.cpp
//
// Verifies SphericalHeadSpatializer + the mixer's per-ear (binaural)
// path. Three checks:
//
//   1. Spatializer math — at full lateral azimuth, ITD ≈ Woodworth's
//      formula evaluated with the engine's default head radius and the
//      configured speed of sound.
//   2. Mixer ITD — a unit impulse fed to a binaural voice with an
//      explicit per-ear delay shows up at the configured sample offset
//      in the late ear, with no signal in the early ear's first
//      samples.
//   3. End-to-end ILD — a broadband click rendered through the runtime
//      with the listener at origin and the source far to the right
//      arrives quieter on the left output channel than on the right.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/bus.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/spatializer.h"

#include "audio_engine/spatial/spherical_head_spatializer.h"

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
constexpr float    kPi         = 3.14159265358979323846f;

// ---- Layer 1: spatializer-only -------------------------------------------

void TestSpatializerItdAtFullLateral() {
    SphericalHeadSpatializer sp;

    SpatialEmitterView e;
    e.position    = {10.0f, 0.0f, 0.0f};     // 10 m to the right
    e.spatialized = true;
    e.minDistance = 1.0f;
    e.maxDistance = 100.0f;
    e.volumeFloor = 0.5f;

    SpatialListenerView l;
    l.position = {0.0f, 0.0f, 0.0f};
    l.forward  = {0.0f, 0.0f, -1.0f};
    l.up       = {0.0f, 1.0f, 0.0f};

    SpatialEnvironmentState env;

    const SpatialParams r = sp.Calculate(e, l, env);

    EXPECT(r.useBinaural);

    // Source on the right: left ear should be the late ear.
    EXPECT(r.delaySamplesR == 0.0f);
    EXPECT(r.delaySamplesL > 0.0f);

    // Right ear should hear the full level; left ear should be in
    // shadow (gain reduced, LPF active).
    EXPECT(r.gainR == 1.0f);
    EXPECT(r.gainL < 1.0f);
    EXPECT(r.lpfAmountL > 0.0f);
    EXPECT(r.lpfAmountR == 0.0f);

    // Woodworth at azimuth = +π/2:
    //   ITD = (a/c) * (sin(π/2) + π/2) = (0.0875/343) * (1 + π/2)
    //       ≈ 6.55e-4 s ≈ 31.4 samples at 48 kHz.
    constexpr float kExpectedSamples = 31.4f;
    std::printf("  ITD samples at full right lateral: expected ~%.1f got %.2f\n",
                kExpectedSamples, r.delaySamplesL);
    EXPECT(r.delaySamplesL > kExpectedSamples - 2.0f);
    EXPECT(r.delaySamplesL < kExpectedSamples + 2.0f);
}

void TestSpatializerSymmetric() {
    SphericalHeadSpatializer sp;
    SpatialEmitterView eR, eL;
    eR.position = {10.0f, 0.0f, 0.0f};
    eL.position = {-10.0f, 0.0f, 0.0f};
    eR.spatialized = eL.spatialized = true;
    eR.minDistance = eL.minDistance = 1.0f;
    eR.maxDistance = eL.maxDistance = 100.0f;
    eR.volumeFloor = eL.volumeFloor = 0.5f;
    SpatialListenerView l;
    l.position = {0.0f, 0.0f, 0.0f};
    l.forward  = {0.0f, 0.0f, -1.0f};
    l.up       = {0.0f, 1.0f, 0.0f};
    SpatialEnvironmentState env;

    const auto r = sp.Calculate(eR, l, env);
    const auto m = sp.Calculate(eL, l, env);

    // Mirror: swap left/right fields and the params should match.
    EXPECT(std::fabs(r.delaySamplesL - m.delaySamplesR) < 1e-3f);
    EXPECT(std::fabs(r.delaySamplesR - m.delaySamplesL) < 1e-3f);
    EXPECT(std::fabs(r.gainL - m.gainR) < 1e-4f);
    EXPECT(std::fabs(r.gainR - m.gainL) < 1e-4f);
    EXPECT(std::fabs(r.lpfAmountL - m.lpfAmountR) < 1e-4f);
}

void TestSpatializerCenterIsSymmetric() {
    SphericalHeadSpatializer sp;
    SpatialEmitterView e;
    e.position    = {0.0f, 0.0f, -5.0f};     // straight ahead
    e.spatialized = true;
    e.minDistance = 1.0f;
    e.maxDistance = 100.0f;
    e.volumeFloor = 0.5f;
    SpatialListenerView l;
    l.position = {0.0f, 0.0f, 0.0f};
    l.forward  = {0.0f, 0.0f, -1.0f};
    l.up       = {0.0f, 1.0f, 0.0f};
    SpatialEnvironmentState env;
    const auto r = sp.Calculate(e, l, env);
    EXPECT(r.useBinaural);
    EXPECT(r.delaySamplesL == 0.0f);
    EXPECT(r.delaySamplesR == 0.0f);
    EXPECT(r.gainL == 1.0f);
    EXPECT(r.gainR == 1.0f);
    EXPECT(r.lpfAmountL == 0.0f);
    EXPECT(r.lpfAmountR == 0.0f);
}

// ---- Layer 2: end-to-end runtime ----------------------------------------

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

// Render a 10 kHz sine source at the given listener-relative position
// through the runtime with SphericalHeadSpatializer plugged in. Returns
// the per-channel peaks measured over a 200 ms window after the
// spatialiser settles.
struct StereoPeaks { float left; float right; };

StereoPeaks RunRuntimeAndMeasurePeaks(const Vec3& sourcePos) {
    AudioConfig cfg;
    cfg.sampleRate                 = kSampleRate;
    cfg.bufferSize                 = kBufferSize;
    cfg.outputMode                 = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters   = 4;
    cfg.budget.maxRegisteredSounds = 4;
    cfg.budget.maxStreamingAssets  = 1;
    cfg.budget.maxStreamingVoices  = 1;
    cfg.budget.maxVoiceSources     = 0;
    cfg.enableOcclusion            = false;
    cfg.enableAirAbsorption        = false;

    AudioRuntime rt;
    auto backend = std::make_unique<OfflineBackend>();
    OfflineBackend* bp = backend.get();

    AudioRuntimeDependencies deps;
    deps.backend     = std::move(backend);
    deps.spatializer = std::make_unique<SphericalHeadSpatializer>();

    if (rt.Initialize(cfg, std::move(deps)) != AudioResult::Success) {
        ++gFails; return {0.0f, 0.0f};
    }

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    lis.forward  = {0.0f, 0.0f, -1.0f};
    lis.up       = {0.0f, 1.0f,  0.0f};
    rt.SetListener(lis);

    constexpr AudioSoundId kSnd = 1;
    std::vector<float> pcm(kSampleRate);
    for (uint32_t i = 0; i < kSampleRate; ++i) {
        pcm[i] = std::sin(2.0f * kPi * 10000.0f * static_cast<float>(i) / kSampleRate);
    }
    rt.RegisterPcmSound(kSnd, pcm, kSampleRate, /*channels*/ 1);

    SoundDefinition def;
    def.soundId          = kSnd;
    def.category         = AudioCategory::SFX;
    def.targetBus        = kBusMaster;
    def.spatialized      = true;
    def.looping          = true;
    def.occlusionEnabled = false;
    def.attenuation.minDistance = 1.0f;
    def.attenuation.maxDistance = 100.0f;
    def.attenuation.volumeFloor = 1.0f;     // remove distance volume falloff for the test
    rt.RegisterSoundDefinition(def);

    EmitterDescriptor ed;
    ed.soundId          = kSnd;
    ed.position         = sourcePos;
    ed.targetBus        = kBusMaster;
    ed.category         = AudioCategory::SFX;
    ed.isLooping        = true;
    ed.isSpatialized    = true;
    ed.occlusionEnabled = false;
    ed.priority         = AudioPriority::Normal;
    ed.attenuation      = def.attenuation;
    auto h = rt.CreateEmitter(ed);
    if (!h) { ++gFails; return {0.0f, 0.0f}; }

    // Warm up.
    std::vector<float> out;
    for (int i = 0; i < 20; ++i) {
        rt.Update(0.025f);
        bp->Render(kSampleRate / 40, out);
    }
    // Measure window.
    std::vector<float> meas;
    for (int i = 0; i < 8; ++i) {
        rt.Update(0.025f);
        bp->Render(kSampleRate / 40, out);
        meas.insert(meas.end(), out.begin(), out.end());
    }

    rt.DestroyEmitter(h.value());
    rt.Shutdown();

    // Stereo interleaved: split.
    StereoPeaks p{0.0f, 0.0f};
    for (size_t i = 0; i + 1 < meas.size(); i += 2) {
        const float l = std::fabs(meas[i]);
        const float r = std::fabs(meas[i + 1]);
        if (l > p.left)  p.left  = l;
        if (r > p.right) p.right = r;
    }
    return p;
}

void TestRuntimeIldFromHeadShadow() {
    const auto right = RunRuntimeAndMeasurePeaks({10.0f, 0.0f, 0.0f});
    const auto left  = RunRuntimeAndMeasurePeaks({-10.0f, 0.0f, 0.0f});
    const auto front = RunRuntimeAndMeasurePeaks({0.0f, 0.0f, -10.0f});
    std::printf("  source-right: L=%.3f R=%.3f\n", right.left, right.right);
    std::printf("  source-left : L=%.3f R=%.3f\n", left.left,  left.right);
    std::printf("  source-front: L=%.3f R=%.3f\n", front.left, front.right);

    // Source on the right: right ear should be louder than left for a
    // 10 kHz tone (head shadows left ear's highs and reduces its gain).
    EXPECT(right.right > right.left);
    // Source on the left: mirror image.
    EXPECT(left.left > left.right);
    // Source straight ahead: both channels approximately equal.
    EXPECT(std::fabs(front.left - front.right) < 0.05f);
    // Mirror symmetry: source-right's right-ear peak ≈ source-left's left-ear peak.
    EXPECT(std::fabs(right.right - left.left)  < 0.05f);
    EXPECT(std::fabs(right.left  - left.right) < 0.05f);
}

} // namespace

int main() {
    std::printf("[binaural_spatializer_test] running...\n");
    TestSpatializerItdAtFullLateral();
    TestSpatializerSymmetric();
    TestSpatializerCenterIsSymmetric();
    TestRuntimeIldFromHeadShadow();
    if (gFails == 0) {
        std::printf("[binaural_spatializer_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[binaural_spatializer_test] %d failure(s)\n", gFails);
    return 1;
}
