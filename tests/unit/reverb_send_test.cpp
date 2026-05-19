// tests/unit/reverb_send_test.cpp
//
// Three layers:
//
//   1. ReverbEffect impulse response; feed a single-sample impulse into
//      the effect's Process, render silence afterwards, confirm a tail
//      exists and decays toward zero. This isolates the DSP from any
//      send-routing concerns.
//
//   2. Voice → reverb bus routing; set up an AudioRuntime with a reverb
//      bus carrying a ReverbEffect, configure globalReverbSend > 0, play
//      a short impulse-like sound, and confirm there is non-trivial
//      audio AFTER the dry signal would have ended.
//
//   3. No-reverb-bus is a no-op; same scenario but without a kBusReverb
//      bus in the graph. The mixer must skip the send entirely; output
//      after the dry sound ends is silence.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/bus.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/types.h"

#include "audio_engine/dsp/reverb_effect.h"

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

float Rms(const float* d, size_t n) {
    if (n == 0) return 0.0f;
    double a = 0.0;
    for (size_t i = 0; i < n; ++i) a += static_cast<double>(d[i]) * d[i];
    return static_cast<float>(std::sqrt(a / static_cast<double>(n)));
}

// -------------------------------------------------------------------------
// Layer 1; DSP impulse response
// -------------------------------------------------------------------------

void TestReverbEffectImpulseResponse() {
    // v0.29.0: ReverbEffect ctor went from 3 args to 6 (Dattorro plate
    // parameter surface). Old roomSize → new decay 1:1, old damping →
    // new hf_damping 1:1 (the soft-migration mapping from EffectConfig).
    // The three new args (predelayMs, lfDamping, diffusion) get the
    // EffectConfig defaults from include/audio_engine/bus.h.
    ReverbEffect rv(
            /*predelayMs*/ 30.0f,
            /*decay*/      0.85f,    // was: roomSize
            /*lfDamping*/  0.0f,
            /*hfDamping*/  0.4f,     // was: damping
            /*diffusion*/  0.625f,
            /*wetGainDb*/  0.0f);
    rv.Prepare(kSampleRate, /*channels*/ 2);

    // Single-sample stereo impulse, then 1 second of silence.
    constexpr uint32_t kFrames = kSampleRate;     // 1.0 s
    std::vector<float> buf(kFrames * 2, 0.0f);
    buf[0] = 1.0f;     // L
    buf[1] = 1.0f;     // R

    // Process in chunks the size of a real callback.
    for (uint32_t off = 0; off < kFrames; off += kBufferSize) {
        const uint32_t n = std::min<uint32_t>(kBufferSize, kFrames - off);
        rv.Process(buf.data() + off * 2, n, /*channels*/ 2,
                    /*sidechain*/ nullptr, /*sidechainCh*/ 0);
    }

    // Energy in the first 50 ms (early reflections + initial buildup) and
    // in 400-600 ms (decaying tail). Both should be non-zero and the latter
    // should be smaller than the former; otherwise the tail isn't decaying.
    const uint32_t early0 = 0;
    const uint32_t early1 = kSampleRate / 20;          // 50 ms
    const uint32_t late0  = kSampleRate * 4 / 10;      // 400 ms
    const uint32_t late1  = kSampleRate * 6 / 10;      // 600 ms

    const float earlyRms = Rms(buf.data() + early0 * 2, (early1 - early0) * 2);
    const float lateRms  = Rms(buf.data() + late0  * 2, (late1  - late0)  * 2);

    std::printf("  ReverbEffect impulse: 0-50ms rms=%.5f  400-600ms rms=%.5f\n",
                earlyRms, lateRms);

    EXPECT(earlyRms > 1e-4f);              // tail must produce energy
    EXPECT(lateRms  > 1e-5f);              // still decaying, not zeroed yet
    EXPECT(lateRms  < earlyRms);           // monotonic-ish decay
}

void TestReverbEffectShorterRoomDecaysFaster() {
    // Both reverbs use the same hf_damping/lfDamping/diffusion so that the
    // ONLY difference is decay length — the test asserts that bigger decay
    // ⇒ longer tail, so it's important the other axes don't vary.
    ReverbEffect big  (/*predelayMs*/ 30.0f, /*decay*/ 0.95f,
                       /*lfDamping*/   0.0f, /*hfDamping*/ 0.2f,
                       /*diffusion*/   0.625f, /*wetGainDb*/ 0.0f);
    ReverbEffect small(/*predelayMs*/ 30.0f, /*decay*/ 0.30f,
                       /*lfDamping*/   0.0f, /*hfDamping*/ 0.2f,
                       /*diffusion*/   0.625f, /*wetGainDb*/ 0.0f);
    big.Prepare  (kSampleRate, 2);
    small.Prepare(kSampleRate, 2);

    constexpr uint32_t kFrames = kSampleRate / 2;   // 0.5 s
    std::vector<float> b1(kFrames * 2, 0.0f);
    std::vector<float> b2(kFrames * 2, 0.0f);
    b1[0] = b1[1] = 1.0f;
    b2[0] = b2[1] = 1.0f;

    for (uint32_t off = 0; off < kFrames; off += kBufferSize) {
        const uint32_t n = std::min<uint32_t>(kBufferSize, kFrames - off);
        big.Process  (b1.data() + off * 2, n, 2, nullptr, 0);
        small.Process(b2.data() + off * 2, n, 2, nullptr, 0);
    }

    // Compare RMS in the late window: the larger room should still have
    // more energy left at 400 ms than the smaller one.
    const uint32_t late0 = kSampleRate * 4 / 10;     // 400 ms
    const uint32_t late1 = kSampleRate * 5 / 10;     // 500 ms
    const float bigLate   = Rms(b1.data() + late0 * 2, (late1 - late0) * 2);
    const float smallLate = Rms(b2.data() + late0 * 2, (late1 - late0) * 2);

    std::printf("  decay@400ms: bigRoom rms=%.5f  smallRoom rms=%.5f\n",
                bigLate, smallLate);
    EXPECT(bigLate > smallLate * 1.5f);
}

// -------------------------------------------------------------------------
// Runtime offline backend (same pattern used elsewhere in this suite)
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
// Layer 2; runtime with a reverb bus + non-zero send
// Layer 3; same, but no reverb bus configured
// -------------------------------------------------------------------------

struct RunResult {
    float drySilenceRms;     // RMS during the silence window after a short
                              // impulse-like sound has finished
};

RunResult RunImpulseAndMeasure(bool addReverbBus, float globalSend) {
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
    cfg.globalReverbSend           = globalSend;

    if (addReverbBus) {
        cfg.busGraph.busCount = 2;
        // [0] master
        cfg.busGraph.buses[0].id     = kBusMaster;
        cfg.busGraph.buses[0].parent = kBusMaster;     // ignored for master
        // [1] reverb → master, with a single ReverbEffect
        cfg.busGraph.buses[1].id     = kBusReverb;
        cfg.busGraph.buses[1].parent = kBusMaster;
        cfg.busGraph.buses[1].outputGainDb = 0.0f;
        cfg.busGraph.buses[1].effects[0].kind             = EffectKind::Reverb;
        cfg.busGraph.buses[1].effects[0].reverbDecay      = 0.9f;
        cfg.busGraph.buses[1].effects[0].reverbHfDamping  = 0.3f;
        cfg.busGraph.buses[1].effects[0].reverbWetGainDb  = 0.0f;
        cfg.busGraph.buses[1].effectCount                 = 1;
    }

    AudioRuntime rt;
    auto backend = std::make_unique<OfflineBackend>();
    OfflineBackend* bp = backend.get();

    AudioRuntimeDependencies deps;
    deps.backend = std::move(backend);
    if (rt.Initialize(cfg, std::move(deps)) != AudioResult::Success) {
        ++gFails; return {0.0f};
    }

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    rt.SetListener(lis);

    // A short noisy click, ~100 ms long. Looping must be off so it ends and
    // we get clean silence afterwards (in the no-reverb case). The duration
    // is deliberately longer than one Update tick (25 ms) so TickOneShots
    // doesn't expire the voice in the same tick it was created; that
    // would race the render callback and produce zero dry signal.
    constexpr AudioSoundId kSnd = 1;
    constexpr uint32_t kClickFrames = kSampleRate / 10;     // 100 ms
    std::vector<float> pcm(kClickFrames);
    for (uint32_t i = 0; i < kClickFrames; ++i) {
        // Mild noise, descending envelope.
        const float t = 1.0f - static_cast<float>(i) / kClickFrames;
        const uint32_t n = (i * 1664525u + 1013904223u);
        const float ns = (static_cast<float>(n & 0xFFFF) / 32768.0f) - 1.0f;
        pcm[i] = 0.8f * t * ns;
    }
    rt.RegisterPcmSound(kSnd, pcm, kSampleRate, /*channels*/ 1);

    SoundDefinition def;
    def.soundId          = kSnd;
    def.category         = AudioCategory::SFX;
    def.targetBus        = kBusMaster;
    def.spatialized      = true;
    def.looping          = false;
    def.occlusionEnabled = false;
    def.attenuation.minDistance = 1.0f;
    def.attenuation.maxDistance = 100.0f;
    def.attenuation.volumeFloor = 1.0f;
    rt.RegisterSoundDefinition(def);

    // Fire the click via a one-shot event.
    AudioEvent ev;
    ev.type     = AudioEventType::PlaySoundAtLocation;
    ev.soundId  = kSnd;
    ev.position = {1.0f, 0.0f, 0.0f};
    ev.priority = AudioPriority::Normal;
    rt.SubmitEvent(ev);

    // Render: tick + 25 ms callback for several ticks.
    std::vector<float> out;
    constexpr uint32_t kTickMs = 25;
    constexpr uint32_t kFramesPerTick = (kSampleRate * kTickMs) / 1000;

    // Initial 4 ticks (100 ms): the click has fully played out by ~5 ms.
    // Capture the LATER ticks (250-450 ms); well after dry has ended;
    // any RMS there is the reverb tail.
    for (uint32_t i = 0; i < 4; ++i) {
        rt.Update(static_cast<float>(kTickMs) / 1000.0f);
        bp->Render(kFramesPerTick, out);
    }
    // Skip ticks 4-9 (100-250 ms) so we sample firmly inside the tail
    // (or firmly inside silence, depending on whether reverb is present).
    for (uint32_t i = 0; i < 6; ++i) {
        rt.Update(static_cast<float>(kTickMs) / 1000.0f);
        bp->Render(kFramesPerTick, out);
    }
    // Measure ticks 10-17 (250-450 ms).
    std::vector<float> tail;
    for (uint32_t i = 0; i < 8; ++i) {
        rt.Update(static_cast<float>(kTickMs) / 1000.0f);
        bp->Render(kFramesPerTick, out);
        tail.insert(tail.end(), out.begin(), out.end());
    }

    rt.Shutdown();
    return {Rms(tail.data(), tail.size())};
}

void TestRuntimeReverbProducesTail() {
    const auto withRev = RunImpulseAndMeasure(/*addReverbBus*/ true,  /*send*/ 0.6f);
    std::printf("  with reverb bus, globalSend=0.6: tail rms (250-450 ms) = %.5f\n",
                withRev.drySilenceRms);
    EXPECT(withRev.drySilenceRms > 1e-4f);
}

void TestRuntimeNoReverbBusIsSilent() {
    const auto noRev = RunImpulseAndMeasure(/*addReverbBus*/ false, /*send*/ 0.6f);
    std::printf("  no reverb bus, globalSend=0.6: tail rms (250-450 ms) = %.6f\n",
                noRev.drySilenceRms);
    EXPECT(noRev.drySilenceRms < 1e-5f);
}

void TestRuntimeZeroSendIsSilent() {
    const auto zero = RunImpulseAndMeasure(/*addReverbBus*/ true,  /*send*/ 0.0f);
    std::printf("  reverb bus present, globalSend=0.0: tail rms = %.6f\n",
                zero.drySilenceRms);
    EXPECT(zero.drySilenceRms < 1e-5f);
}

} // namespace

int main() {
    std::printf("[reverb_send_test] running...\n");
    TestReverbEffectImpulseResponse();
    TestReverbEffectShorterRoomDecaysFaster();
    TestRuntimeReverbProducesTail();
    TestRuntimeNoReverbBusIsSilent();
    TestRuntimeZeroSendIsSilent();
    if (gFails == 0) {
        std::printf("[reverb_send_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[reverb_send_test] %d failure(s)\n", gFails);
    return 1;
}
