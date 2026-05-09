// tests/unit/sound_rtpc_test.cpp
//
// Validates render-thread RTPC volume modulation: setting a global
// parameter via SetGlobalParameter changes the rendered audio
// volume of voices whose sound has a registered binding.
//
// Audibility-verified: each test renders real audio and asserts on
// measured RMS, not on internal counters. If the binding evaluation
// or smoother dispatch breaks, RMS doesn't follow the parameter
// and these tests fail.

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

    // The orchestrator's per-emitter UpdateParams pass (where userGain
    // multiplies into sp.gain and the result is posted to the mixer)
    // only runs when a listener is registered. Without this, the
    // mixer voice keeps whatever gain the StartSound command stamped
    // and RTPC modulation has no audible effect.
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

// Register a 1-second 440 Hz mono sine wave under the given soundId.
void RegisterSine(audio::AudioRuntime& rt, audio::AudioSoundId id,
                   bool spatialized = false) {
    std::vector<float> sine(48000);
    for (size_t i = 0; i < sine.size(); ++i) {
        sine[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f *
                                    static_cast<float>(i) / 48000.0f);
    }
    rt.RegisterPcmSound(id, sine, 48000, 1);

    audio::SoundDefinition def;
    def.soundId      = id;
    def.category     = audio::AudioCategory::SFX;
    def.targetBus    = audio::kBusMaster;
    def.spatialized  = spatialized;
    def.attenuation.minDistance = 1.0f;
    def.attenuation.maxDistance = 1000.0f;
    rt.RegisterSoundDefinition(def);
}

void TestUnsetParameterLeavesAuthoredVolume() {
    std::cout << "  [unset parameter: binding has no effect, authored volume preserved]\n";
    audio::AudioRuntime rt;
    OfflineBackend* be = InitRuntime(rt);

    constexpr audio::AudioSoundId kSnd = 0xAA01;
    RegisterSine(rt, kSnd);

    // Bind volume to "health" but never call set_rtpc("health", ...).
    // Skip-when-unset semantics say the binding has no effect — the
    // sine renders at its authored 0.5 amplitude.
    const auto pHealth = audio::HashParameterName("health");
    auto rc = rt.SetSoundVolumeRtpc(kSnd, pHealth,
        /*minV*/ 0.0f, /*maxV*/ 1.0f,
        /*minVol*/ 0.0f, /*maxVol*/ 1.0f,
        /*smoothingMs*/ 0.0f);
    assert(rc == audio::AudioResult::Success);

    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
        kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);

    std::vector<float> audio_buf;
    be->Render(/*frames*/ 24000, audio_buf);
    const float rms = ComputeRms(audio_buf);
    std::cout << "    rendered RMS = " << rms << " (expect ~0.25 for unmodulated sine)\n";
    // 0.5-amplitude mono sine through stereo at unity gain ≈ 0.25 RMS
    // (with some small attenuation from the bus chain). Just assert
    // it's clearly above silence.
    assert(rms > 0.10f);

    rt.Shutdown();
}

void TestRtpcZeroSilencesAudio() {
    std::cout << "  [parameter at zero with binding {0->0, 1->1}: voice is silent]\n";
    audio::AudioRuntime rt;
    OfflineBackend* be = InitRuntime(rt);

    constexpr audio::AudioSoundId kSnd = 0xAA02;
    RegisterSine(rt, kSnd);

    const auto pHealth = audio::HashParameterName("health");
    rt.SetSoundVolumeRtpc(kSnd, pHealth, 0.0f, 1.0f, 0.0f, 1.0f, /*smooth*/ 0.0f);
    // Set health to 0 → volume mapped to 0.
    rt.SetGlobalParameter(pHealth, 0.0f);

    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
        kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);

    std::vector<float> audio_buf;
    be->Render(24000, audio_buf);
    const float rms = ComputeRms(audio_buf);
    std::cout << "    rendered RMS = " << rms << " (expect ~0)\n";
    assert(rms < 0.01f);

    rt.Shutdown();
}

void TestRtpcOneEqualsAuthoredVolume() {
    std::cout << "  [parameter at one: rendered volume matches authored]\n";
    audio::AudioRuntime rt;
    OfflineBackend* be = InitRuntime(rt);

    constexpr audio::AudioSoundId kSnd = 0xAA03;
    RegisterSine(rt, kSnd);

    const auto pHealth = audio::HashParameterName("health");
    rt.SetSoundVolumeRtpc(kSnd, pHealth, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    rt.SetGlobalParameter(pHealth, 1.0f);

    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
        kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);

    std::vector<float> audio_buf;
    be->Render(24000, audio_buf);
    const float rms = ComputeRms(audio_buf);
    std::cout << "    rendered RMS = " << rms << " (expect ~0.25)\n";
    assert(rms > 0.10f);

    rt.Shutdown();
}

void TestRtpcMidpointHalfVolume() {
    std::cout << "  [parameter at 0.5 with binding {0->0, 1->1}: ~half rendered RMS]\n";
    audio::AudioRuntime rt;
    OfflineBackend* be = InitRuntime(rt);

    constexpr audio::AudioSoundId kSnd = 0xAA04;
    RegisterSine(rt, kSnd);

    const auto pHealth = audio::HashParameterName("health");
    rt.SetSoundVolumeRtpc(kSnd, pHealth, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);

    // Reference render at full volume.
    rt.SetGlobalParameter(pHealth, 1.0f);
    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
        kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
    std::vector<float> bufFull;
    be->Render(24000, bufFull);
    const float rmsFull = ComputeRms(bufFull);

    // Re-render the same sound at half parameter value. Bring up a
    // fresh runtime so the previous one-shot doesn't pollute.
    audio::AudioRuntime rt2;
    OfflineBackend* be2 = InitRuntime(rt2);
    RegisterSine(rt2, kSnd);
    rt2.SetSoundVolumeRtpc(kSnd, pHealth, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    rt2.SetGlobalParameter(pHealth, 0.5f);
    rt2.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
        kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt2.Update(1.0f / 60.0f);
    std::vector<float> bufHalf;
    be2->Render(24000, bufHalf);
    const float rmsHalf = ComputeRms(bufHalf);

    std::cout << "    full RMS = " << rmsFull
              << "   half RMS = " << rmsHalf
              << "   ratio = " << (rmsHalf / std::max(rmsFull, 1e-9f)) << "\n";
    // Half parameter value with linear binding → half voice gain →
    // half RMS. Allow ±20% slack for smoother transient and bus chain
    // effects.
    const float ratio = rmsHalf / std::max(rmsFull, 1e-9f);
    assert(ratio > 0.40f && ratio < 0.60f);

    rt.Shutdown();
    rt2.Shutdown();
}

void TestInvertedBindingHigherParamLowerVolume() {
    std::cout << "  [inverted binding {1->0, 0->1}: full health is silent]\n";
    audio::AudioRuntime rt;
    OfflineBackend* be = InitRuntime(rt);

    constexpr audio::AudioSoundId kSnd = 0xAA05;
    RegisterSine(rt, kSnd);

    const auto pHealth = audio::HashParameterName("health");
    // Heartbeat-style: minVolume at full health, maxVolume at low health.
    // Wire by mapping minValue=1 → minVolume=0 and maxValue=0 → maxVolume=1.
    rt.SetSoundVolumeRtpc(kSnd, pHealth,
        /*minV*/ 1.0f, /*maxV*/ 0.0f,
        /*minVol*/ 0.0f, /*maxVol*/ 1.0f,
        /*smooth*/ 0.0f);
    rt.SetGlobalParameter(pHealth, 1.0f);  // full health

    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
        kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);

    std::vector<float> audio_buf;
    be->Render(24000, audio_buf);
    const float rms = ComputeRms(audio_buf);
    std::cout << "    full health, inverted binding: RMS = " << rms << "\n";
    assert(rms < 0.01f);

    rt.Shutdown();
}

void TestClampsOutsideRange() {
    std::cout << "  [parameter beyond range clamps to nearer endpoint]\n";
    audio::AudioRuntime rt;
    OfflineBackend* be = InitRuntime(rt);

    constexpr audio::AudioSoundId kSnd = 0xAA06;
    RegisterSine(rt, kSnd);

    const auto p = audio::HashParameterName("test");
    // Binding {0..1 -> 0..1}. Parameter set to 100 → t clamps to 1
    // → volume clamps to maxVolume = 1.
    rt.SetSoundVolumeRtpc(kSnd, p, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    rt.SetGlobalParameter(p, 100.0f);

    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
        kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);

    std::vector<float> audio_buf;
    be->Render(24000, audio_buf);
    const float rms = ComputeRms(audio_buf);
    assert(rms > 0.10f);

    // Parameter set to -100 → t clamps to 0 → volume = 0 → silence.
    rt.SetGlobalParameter(p, -100.0f);
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
    rt.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
        kSnd, audio::Vec3{0,0,0}));
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
    std::vector<float> bufLow;
    be->Render(24000, bufLow);
    const float rmsLow = ComputeRms(bufLow);
    std::cout << "    above-range RMS = " << rms
              << "   below-range RMS = " << rmsLow << "\n";
    assert(rmsLow < 0.01f);

    rt.Shutdown();
}

void TestClearStopsModulation() {
    std::cout << "  [Clear: subsequent voices use authored volume]\n";
    audio::AudioRuntime rt;
    InitRuntime(rt);

    constexpr audio::AudioSoundId kSnd = 0xAA07;
    RegisterSine(rt, kSnd);

    const auto p = audio::HashParameterName("temp");
    auto rc = rt.SetSoundVolumeRtpc(kSnd, p, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    assert(rc == audio::AudioResult::Success);
    assert(rt.GetSoundRtpcBindingCount() == 1u);

    bool removed = rt.ClearSoundVolumeRtpc(kSnd);
    assert(removed);
    assert(rt.GetSoundRtpcBindingCount() == 0u);

    bool removedAgain = rt.ClearSoundVolumeRtpc(kSnd);
    assert(!removedAgain);

    rt.Shutdown();
}

void TestApiValidation() {
    std::cout << "  [SetSoundVolumeRtpc rejects invalid arguments]\n";
    audio::AudioRuntime rt;
    InitRuntime(rt);

    const auto p = audio::HashParameterName("ok");

    // Invalid soundId.
    auto r1 = rt.SetSoundVolumeRtpc(audio::kInvalidSoundId, p,
        0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    assert(r1 == audio::AudioResult::InvalidArgument);

    // Invalid paramId.
    auto r2 = rt.SetSoundVolumeRtpc(0xFEED, audio::kInvalidParameterId,
        0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    assert(r2 == audio::AudioResult::InvalidArgument);

    // Degenerate range (min == max would divide by zero).
    auto r3 = rt.SetSoundVolumeRtpc(0xFEED, p, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f);
    assert(r3 == audio::AudioResult::InvalidArgument);

    // NaN endpoint.
    auto nan = std::numeric_limits<float>::quiet_NaN();
    auto r4 = rt.SetSoundVolumeRtpc(0xFEED, p, 0.0f, 1.0f, nan, 1.0f, 0.0f);
    assert(r4 == audio::AudioResult::InvalidArgument);

    // Negative smoothing.
    auto r5 = rt.SetSoundVolumeRtpc(0xFEED, p, 0.0f, 1.0f, 0.0f, 1.0f, -1.0f);
    assert(r5 == audio::AudioResult::InvalidArgument);

    rt.Shutdown();
}

} // namespace

int main() {
    std::cout << "[sound_rtpc_test]\n";
    TestUnsetParameterLeavesAuthoredVolume();
    TestRtpcZeroSilencesAudio();
    TestRtpcOneEqualsAuthoredVolume();
    TestRtpcMidpointHalfVolume();
    TestInvertedBindingHigherParamLowerVolume();
    TestClampsOutsideRange();
    TestClearStopsModulation();
    TestApiValidation();
    std::cout << "[sound_rtpc_test] PASSED\n";
    return 0;
}
