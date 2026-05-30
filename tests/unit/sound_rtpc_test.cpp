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

// tests/unit/sound_rtpc_test.cpp
//
// Validates render-thread RTPC modulation (v0.5 multi-target API):
//   * Volume / Pitch / LowPass targets (ReverbSend covered separately
//     when a reverb bus is configured)
//   * Linear / Exponential / InverseExponential / SCurve curves
//   * Multiple bindings per sound (volume + pitch independently driven)
//   * Skip-when-unset semantics, clamping, clear, API validation
//   * Audibility-verified end-to-end: real audio rendered, RMS measured

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/sound_bank.h"
#include "audio_engine/types.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <vector>

namespace {

class OfflineBackend final : public audio::IAudioBackend {
public:
    audio::AudioResult Start(const audio::AudioBackendConfig& cfg,
                              audio::IAudioRenderCallback*    cb) override {
        cfg_ = cfg; cb_ = cb;
        scratch_.assign(static_cast<size_t>(cfg.bufferSize) * cfg.channels, 0.0f);
        return audio::AudioResult::Success;
    }
    void     Stop() override { cb_ = nullptr; }
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "OfflineRtpc"; }

    void Render(uint32_t frames, std::vector<float>& out) {
        out.clear();
        if (!cb_) return;
        const uint32_t bs = cfg_.bufferSize, ch = cfg_.channels;
        out.reserve(static_cast<size_t>(frames + bs) * ch);
        uint32_t produced = 0;
        while (produced < frames) {
            const uint32_t take = std::min<uint32_t>(bs, frames - produced);
            std::fill(scratch_.begin(), scratch_.end(), 0.0f);
            cb_->OnRender(scratch_.data(), take, ch);
            out.insert(out.end(), scratch_.begin(),
                       scratch_.begin() + static_cast<ptrdiff_t>(take * ch));
            produced += take;
        }
    }

private:
    audio::AudioBackendConfig    cfg_{};
    audio::IAudioRenderCallback* cb_ = nullptr;
    std::vector<float>           scratch_;
};

OfflineBackend* InitRuntime(audio::AudioRuntime& rt) {
    audio::AudioConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    cfg.outputMode = audio::AudioOutputMode::Stereo;

    auto backend = std::make_unique<OfflineBackend>();
    OfflineBackend* raw = backend.get();
    audio::AudioRuntimeDependencies deps;
    deps.backend = std::move(backend);
    auto rc = rt.Initialize(cfg, std::move(deps));
    assert(rc == audio::AudioResult::Success);

    audio::AudioListener lis;
    lis.position = {0, 0, 0};
    lis.forward  = {0, 0, -1};
    lis.up       = {0, 1, 0};
    rt.SetListener(lis);
    return raw;
}

float ComputeRms(const std::vector<float>& buf) {
    if (buf.empty()) return 0.0f;
    double acc = 0.0;
    for (float s : buf) acc += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(acc / static_cast<double>(buf.size())));
}

void RegisterSine(audio::AudioRuntime& rt, audio::AudioSoundId id,
                   float frequencyHz = 440.0f) {
    std::vector<float> sine(48000);
    for (size_t i = 0; i < sine.size(); ++i) {
        sine[i] = 0.5f * std::sin(2.0f * 3.14159265f * frequencyHz *
                                    static_cast<float>(i) / 48000.0f);
    }
    rt.RegisterPcmSound(id, sine, 48000, 1);

    audio::SoundDefinition def;
    def.soundId      = id;
    def.category     = audio::AudioCategory::SFX;
    def.targetBus    = audio::kBusMaster;
    def.spatialized  = false;
    def.attenuation.minDistance = 1.0f;
    def.attenuation.maxDistance = 1000.0f;
    rt.RegisterSoundDefinition(def);
}

audio::SoundRtpcBinding MakeBinding(audio::AudioParameterId paramId,
                                      audio::RtpcTarget target,
                                      float minValue, float maxValue,
                                      float minOutput, float maxOutput,
                                      audio::RtpcCurve curve = audio::RtpcCurve::Linear,
                                      float exponent = 2.0f,
                                      float smoothingMs = 0.0f) {
    audio::SoundRtpcBinding b;
    b.paramId       = paramId;
    b.target        = target;
    b.curve         = curve;
    b.curveExponent = exponent;
    b.minValue      = minValue;
    b.maxValue      = maxValue;
    b.minOutput     = minOutput;
    b.maxOutput     = maxOutput;
    b.smoothingMs   = smoothingMs;
    return b;
}

// --- Volume target ---------------------------------------------------------

void TestVolumeBindingDrivesRenderedRms() {
    std::cout << "  [volume binding: RMS halves at parameter midpoint]\n";

    const auto p = audio::HashParameterName("health");
    constexpr audio::AudioSoundId kSnd = 0xB001;

    audio::AudioRuntime rt1;
    OfflineBackend* be1 = InitRuntime(rt1);
    RegisterSine(rt1, kSnd);
    rt1.SetSoundRtpc(kSnd, MakeBinding(p, audio::RtpcTarget::Volume, 0, 1, 0, 1));
    rt1.SetGlobalParameter(p, 1.0f);
    rt1.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt1.Update(1.0f / 60.0f);
    std::vector<float> bufFull;
    be1->Render(24000, bufFull);
    const float rmsFull = ComputeRms(bufFull);

    audio::AudioRuntime rt2;
    OfflineBackend* be2 = InitRuntime(rt2);
    RegisterSine(rt2, kSnd);
    rt2.SetSoundRtpc(kSnd, MakeBinding(p, audio::RtpcTarget::Volume, 0, 1, 0, 1));
    rt2.SetGlobalParameter(p, 0.5f);
    rt2.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt2.Update(1.0f / 60.0f);
    std::vector<float> bufHalf;
    be2->Render(24000, bufHalf);
    const float rmsHalf = ComputeRms(bufHalf);

    const float ratio = rmsHalf / std::max(rmsFull, 1e-9f);
    std::cout << "    full RMS = " << rmsFull << "  half RMS = " << rmsHalf
              << "  ratio = " << ratio << "\n";
    assert(rmsFull > 0.10f);
    assert(ratio > 0.40f && ratio < 0.60f);

    rt1.Shutdown(); rt2.Shutdown();
}

// --- Pitch target ----------------------------------------------------------

void TestPitchBindingChangesRender() {
    std::cout << "  [pitch binding: rendered audio differs from reference]\n";

    const auto p = audio::HashParameterName("intensity");
    constexpr audio::AudioSoundId kSnd = 0xB002;

    audio::AudioRuntime rt1;
    OfflineBackend* be1 = InitRuntime(rt1);
    RegisterSine(rt1, kSnd, 880.0f);
    rt1.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt1.Update(1.0f / 60.0f);
    std::vector<float> bufRef;
    be1->Render(24000, bufRef);
    const float rmsRef = ComputeRms(bufRef);

    audio::AudioRuntime rt2;
    OfflineBackend* be2 = InitRuntime(rt2);
    RegisterSine(rt2, kSnd, 880.0f);
    rt2.SetSoundRtpc(kSnd, MakeBinding(p, audio::RtpcTarget::Pitch,
                                         0.0f, 1.0f, 0.5f, 1.0f));
    rt2.SetGlobalParameter(p, 0.0f);  // → pitch = 0.5 (octave down)
    rt2.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt2.Update(1.0f / 60.0f);
    std::vector<float> bufLow;
    be2->Render(24000, bufLow);
    const float rmsLow = ComputeRms(bufLow);

    std::cout << "    ref RMS = " << rmsRef << "  low RMS = " << rmsLow << "\n";
    assert(rmsRef > 0.10f && rmsLow > 0.10f);

    // Distinct waveforms.
    assert(bufRef.size() == bufLow.size());
    double diff = 0.0;
    for (size_t i = 0; i < bufRef.size(); ++i) {
        diff += std::abs(bufRef[i] - bufLow[i]);
    }
    diff /= static_cast<double>(bufRef.size());
    std::cout << "    mean abs diff = " << diff << " (expect noticeably > 0)\n";
    assert(diff > 0.05);

    rt1.Shutdown(); rt2.Shutdown();
}

// --- LowPass target --------------------------------------------------------

void TestLowPassBindingAttenuatesHighFreq() {
    std::cout << "  [lowpass binding: 5 kHz sine attenuated when filter engaged]\n";

    const auto p = audio::HashParameterName("muffle");
    constexpr audio::AudioSoundId kSnd = 0xB003;

    audio::AudioRuntime rt1;
    OfflineBackend* be1 = InitRuntime(rt1);
    RegisterSine(rt1, kSnd, 5000.0f);
    rt1.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt1.Update(1.0f / 60.0f);
    std::vector<float> bufOpen;
    be1->Render(24000, bufOpen);
    const float rmsOpen = ComputeRms(bufOpen);

    audio::AudioRuntime rt2;
    OfflineBackend* be2 = InitRuntime(rt2);
    RegisterSine(rt2, kSnd, 5000.0f);
    rt2.SetSoundRtpc(kSnd, MakeBinding(p, audio::RtpcTarget::LowPassCutoff,
                                         0.0f, 1.0f, 0.0f, 1.0f));
    rt2.SetGlobalParameter(p, 1.0f);
    rt2.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt2.Update(1.0f / 60.0f);
    std::vector<float> bufFiltered;
    be2->Render(24000, bufFiltered);
    const float rmsFiltered = ComputeRms(bufFiltered);

    std::cout << "    open RMS = " << rmsOpen
              << "  filtered RMS = " << rmsFiltered
              << "  ratio = " << (rmsFiltered / std::max(rmsOpen, 1e-9f)) << "\n";
    assert(rmsOpen > 0.10f);
    assert(rmsFiltered < rmsOpen * 0.85f);

    rt1.Shutdown(); rt2.Shutdown();
}

// --- Multiple bindings per sound -------------------------------------------

void TestMultipleBindingsCoexist() {
    std::cout << "  [multi-binding: volume + pitch on same sound, independent]\n";

    audio::AudioRuntime rt;
    OfflineBackend* be = InitRuntime(rt);
    constexpr audio::AudioSoundId kSnd = 0xB004;
    RegisterSine(rt, kSnd, 880.0f);

    const auto pVol   = audio::HashParameterName("ducking");
    const auto pPitch = audio::HashParameterName("doppler_sim");

    auto rcV = rt.SetSoundRtpc(kSnd, MakeBinding(
        pVol, audio::RtpcTarget::Volume, 0.0f, 1.0f, 1.0f, 0.0f));
    assert(rcV == audio::AudioResult::Success);

    auto rcP = rt.SetSoundRtpc(kSnd, MakeBinding(
        pPitch, audio::RtpcTarget::Pitch, 0.0f, 1.0f, 1.0f, 0.5f));
    assert(rcP == audio::AudioResult::Success);

    assert(rt.GetSoundRtpcBindingCount() == 2u);

    rt.SetGlobalParameter(pVol,   0.0f);
    rt.SetGlobalParameter(pPitch, 0.0f);
    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
    std::vector<float> buf;
    be->Render(24000, buf);
    const float rmsFull = ComputeRms(buf);
    std::cout << "    both at min: RMS = " << rmsFull << "\n";
    assert(rmsFull > 0.10f);

    rt.SetGlobalParameter(pVol, 1.0f);
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
    std::vector<float> bufSilent;
    be->Render(24000, bufSilent);
    const float rmsSilent = ComputeRms(bufSilent);
    std::cout << "    volume cranked: RMS = " << rmsSilent << "\n";
    assert(rmsSilent < 0.01f);

    auto rcRebind = rt.SetSoundRtpc(kSnd, MakeBinding(
        pVol, audio::RtpcTarget::Volume, 0.0f, 1.0f, 0.5f, 0.5f));
    assert(rcRebind == audio::AudioResult::Success);
    assert(rt.GetSoundRtpcBindingCount() == 2u);

    bool removedVol = rt.ClearSoundRtpc(kSnd, audio::RtpcTarget::Volume);
    assert(removedVol);
    assert(rt.GetSoundRtpcBindingCount() == 1u);

    size_t removedAll = rt.ClearAllSoundRtpc(kSnd);
    assert(removedAll == 1u);
    assert(rt.GetSoundRtpcBindingCount() == 0u);

    rt.Shutdown();
}

// --- Curves ----------------------------------------------------------------

void TestCurveTypesProduceDifferentOutput() {
    std::cout << "  [curves: linear / exponential / inverse-exp / scurve at midpoint]\n";

    const auto p = audio::HashParameterName("dial");
    constexpr audio::AudioSoundId kSnd = 0xB005;

    auto run_with_curve = [&](audio::RtpcCurve curve) -> float {
        audio::AudioRuntime rt;
        OfflineBackend* be = InitRuntime(rt);
        RegisterSine(rt, kSnd);
        rt.SetSoundRtpc(kSnd, MakeBinding(p, audio::RtpcTarget::Volume,
                                            0.0f, 1.0f, 0.0f, 1.0f, curve));
        rt.SetGlobalParameter(p, 0.5f);
        rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kSnd, audio::Vec3{0,0,0}));
        for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
        std::vector<float> buf;
        be->Render(24000, buf);
        const float rms = ComputeRms(buf);
        rt.Shutdown();
        return rms;
    };

    const float rmsLinear = run_with_curve(audio::RtpcCurve::Linear);
    const float rmsExp    = run_with_curve(audio::RtpcCurve::Exponential);
    const float rmsInvExp = run_with_curve(audio::RtpcCurve::InverseExponential);
    const float rmsScurve = run_with_curve(audio::RtpcCurve::SCurve);

    std::cout << "    linear=" << rmsLinear
              << "  exp(2)=" << rmsExp
              << "  invexp(2)=" << rmsInvExp
              << "  scurve=" << rmsScurve << "\n";

    // Linear at t=0.5 → 0.5 → RMS half.
    // Exponential(2) at t=0.5 → 0.25 → quarter.
    // InverseExponential(2) at t=0.5 → 0.75 → three-quarters.
    // SCurve at t=0.5 → 0.5 (smoothstep is symmetric around midpoint).
    assert(rmsExp < rmsLinear);                       // exp pulls toward min
    assert(rmsInvExp > rmsLinear);                    // inv-exp pulls toward max
    assert(std::abs(rmsScurve - rmsLinear) < 0.02f);  // smoothstep ≈ linear at 0.5
}

// --- Skip-when-unset preserved per-binding ---------------------------------

void TestSkipWhenUnsetIsPerBinding() {
    std::cout << "  [skip-when-unset: one binding's unset param doesn't affect others]\n";

    audio::AudioRuntime rt;
    OfflineBackend* be = InitRuntime(rt);
    constexpr audio::AudioSoundId kSnd = 0xB006;
    RegisterSine(rt, kSnd);

    const auto pSet   = audio::HashParameterName("set_param");
    const auto pUnset = audio::HashParameterName("never_set");

    rt.SetSoundRtpc(kSnd, MakeBinding(pSet, audio::RtpcTarget::Volume,
                                        0, 1, 0, 1));
    rt.SetSoundRtpc(kSnd, MakeBinding(pUnset, audio::RtpcTarget::Pitch,
                                        0, 1, 1, 0.5f));

    rt.SetGlobalParameter(pSet, 0.0f);

    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
    std::vector<float> buf;
    be->Render(24000, buf);
    const float rms = ComputeRms(buf);
    std::cout << "    rendered RMS = " << rms << " (expect ~0)\n";
    assert(rms < 0.01f);

    rt.Shutdown();
}

// --- API validation --------------------------------------------------------

void TestApiValidation() {
    std::cout << "  [SetSoundRtpc rejects invalid arguments]\n";
    audio::AudioRuntime rt;
    InitRuntime(rt);

    const auto p = audio::HashParameterName("ok");

    auto r1 = rt.SetSoundRtpc(audio::kInvalidSoundId,
        MakeBinding(p, audio::RtpcTarget::Volume, 0, 1, 0, 1));
    assert(r1 == audio::AudioResult::InvalidArgument);

    auto r2 = rt.SetSoundRtpc(0xFEED,
        MakeBinding(audio::kInvalidParameterId, audio::RtpcTarget::Volume,
                    0, 1, 0, 1));
    assert(r2 == audio::AudioResult::InvalidArgument);

    auto r3 = rt.SetSoundRtpc(0xFEED,
        MakeBinding(p, audio::RtpcTarget::Volume, 0.5f, 0.5f, 0, 1));
    assert(r3 == audio::AudioResult::InvalidArgument);

    auto nan = std::numeric_limits<float>::quiet_NaN();
    auto bNaN = MakeBinding(p, audio::RtpcTarget::Volume, 0, 1, nan, 1);
    auto r4 = rt.SetSoundRtpc(0xFEED, bNaN);
    assert(r4 == audio::AudioResult::InvalidArgument);

    auto bNegSmooth = MakeBinding(p, audio::RtpcTarget::Volume, 0, 1, 0, 1);
    bNegSmooth.smoothingMs = -1.0f;
    auto r5 = rt.SetSoundRtpc(0xFEED, bNegSmooth);
    assert(r5 == audio::AudioResult::InvalidArgument);

    auto bBadTgt = MakeBinding(p, audio::RtpcTarget::Volume, 0, 1, 0, 1);
    bBadTgt.target = static_cast<audio::RtpcTarget>(99);
    auto r6 = rt.SetSoundRtpc(0xFEED, bBadTgt);
    assert(r6 == audio::AudioResult::InvalidArgument);

    rt.Shutdown();
}

} // namespace

int main() {
    std::cout << "[sound_rtpc_test]\n";
    TestVolumeBindingDrivesRenderedRms();
    TestPitchBindingChangesRender();
    TestLowPassBindingAttenuatesHighFreq();
    TestMultipleBindingsCoexist();
    TestCurveTypesProduceDifferentOutput();
    TestSkipWhenUnsetIsPerBinding();
    TestApiValidation();
    std::cout << "[sound_rtpc_test] PASSED\n";
    return 0;
}
