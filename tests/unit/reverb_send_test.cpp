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
    // v0.29.4: drop decay from 0.85 to 0.50 so this test cleanly
    // separates the tank's buildup phase from its decay phase. The
    // comparative `TestReverbEffectShorterRoomDecaysFaster` test still
    // exercises both extremes (big=0.95, small=0.30); this test is the
    // stability check, so we want a moderate value that's well inside
    // the stable region for any reasonable allpass implementation.
    // (Prior to the v0.29.4 Schroeder write-back fix, decay=0.85 with
    // hf_damping=0.4, lf_damping=0 caused the tank to diverge — the
    // buggy allpass had >1 gain at low frequencies that compounded
    // around the figure-8 loop. See CHANGELOG v0.29.4 for details.)
    ReverbEffect rv(
            /*predelayMs*/ 30.0f,
            /*decay*/      0.50f,
            /*lfDamping*/  0.0f,
            /*hfDamping*/  0.4f,
            /*diffusion*/  0.625f,
            /*dryGainDb*/  -100.0f,  // mute dry to measure wet only
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

    // Plate reverb buildup characteristic:
    // Unlike Freeverb (Schroeder comb topology, energy decays exponentially
    // from t=0), the Dattorro plate's cross-coupled tank takes ~100-300 ms
    // to fully energize. At high decay values the late field is *louder*
    // than the very-early field — that's the plate's diffuse-field
    // buildup, modeling how a real metal plate's surface saturates with
    // standing-wave energy. The original v0.28.x version of this test
    // asserted `lateRms < earlyRms`, which encoded Freeverb's behavior;
    // see CHANGELOG v0.29.3 for the migration note.
    //
    // What we test instead, which is invariant to topology:
    //   1. The reverb produces measurable tail energy after the predelay
    //   2. That energy decays *over time* — measured between two LATE
    //      windows (both past the buildup region), assertion is one-sided.
    //
    // Window placement: predelay is 30 ms in this test, so the first
    // 30 ms of the buffer is silence; wet field starts arriving at 30 ms.
    // Buildup at decay=0.50 reaches peak around 100-150 ms; "mid" (200-
    // 400 ms) captures the post-peak energy, "tail" (700-1000 ms)
    // captures the decay region well past the buildup. With proper
    // allpass behavior (post v0.29.4 fix), per-round-trip loop gain at
    // decay=0.50 is roughly 0.66² ≈ 0.44, giving a clearly bounded and
    // exponentially decaying tail.
    const uint32_t mid0  = kSampleRate * 2 / 10;       // 200 ms (past buildup)
    const uint32_t mid1  = kSampleRate * 4 / 10;       // 400 ms
    const uint32_t tail0 = kSampleRate * 7 / 10;       // 700 ms (decay region)
    const uint32_t tail1 = kSampleRate;                // 1000 ms

    const float midRms  = Rms(buf.data() + mid0  * 2, (mid1  - mid0)  * 2);
    const float tailRms = Rms(buf.data() + tail0 * 2, (tail1 - tail0) * 2);

    std::printf("  ReverbEffect impulse: mid(200-400ms) rms=%.5f  "
                "tail(700-1000ms) rms=%.5f\n", midRms, tailRms);

    EXPECT(midRms  > 1e-4f);          // tail produces measurable energy
    EXPECT(tailRms > 1e-6f);          // ...persists into the deep tail
    EXPECT(tailRms < midRms);         // ...and decays over time (no runaway)
}

void TestReverbEffectShorterRoomDecaysFaster() {
    // Both reverbs use the same hf_damping/lfDamping/diffusion so that the
    // ONLY difference is decay length — the test asserts that bigger decay
    // ⇒ longer tail, so it's important the other axes don't vary.
    ReverbEffect big  (/*predelayMs*/ 30.0f, /*decay*/ 0.95f,
                       /*lfDamping*/   0.0f, /*hfDamping*/ 0.2f,
                       /*diffusion*/   0.625f,
                       /*dryGainDb*/  -100.0f, /*wetGainDb*/ 0.0f);
    ReverbEffect small(/*predelayMs*/ 30.0f, /*decay*/ 0.30f,
                       /*lfDamping*/   0.0f, /*hfDamping*/ 0.2f,
                       /*diffusion*/   0.625f,
                       /*dryGainDb*/  -100.0f, /*wetGainDb*/ 0.0f);
    big.Prepare  (kSampleRate, 2);
    small.Prepare(kSampleRate, 2);

    // v0.58.0: render 1.0 s instead of 0.5 s; the v0.57.0 tank fix
    // means tank circulations are now ~70-83 ms each (the actual
    // delay-line lengths) rather than zero-delay. So meaningful
    // big-vs-small decay separation requires multiple circulations.
    // Pre-v0.57.0 the broken tank looped at sample rate, so at 400 ms
    // the small (decay=0.30) was already dead while big (0.95) was
    // still alive — a 1.5x ratio assertion was easy. Post-fix, the
    // 400 ms window is still buildup-region-dominated for both
    // (~5 circulations), where the decay-independent diffuser energy
    // dominates and the ratio is only ~1.3x.
    //
    // Window sweep at decay 0.95 vs 0.30:
    //   400-500 ms  ratio 1.33  (old window — FAILS post-fix)
    //   500-600 ms  ratio 1.53
    //   700-800 ms  ratio 1.83
    //   800-1000 ms ratio 2.2x  (new window — comfortable margin)
    constexpr uint32_t kFrames = kSampleRate;       // 1.0 s
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
    // more energy left at 800-1000 ms than the smaller one. Window
    // chosen so the diffuser-driven early field has fully decayed and
    // we're measuring the tank-driven sustain that differentiates
    // big from small.
    const uint32_t late0 = kSampleRate * 8 / 10;     // 800 ms
    const uint32_t late1 = kSampleRate;              // 1000 ms
    const float bigLate   = Rms(b1.data() + late0 * 2, (late1 - late0) * 2);
    const float smallLate = Rms(b2.data() + late0 * 2, (late1 - late0) * 2);

    std::printf("  decay@800-1000ms: bigRoom rms=%.5f  smallRoom rms=%.5f  ratio=%.2fx\n",
                bigLate, smallLate, smallLate > 0 ? bigLate / smallLate : 0.0);
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
        // [1] reverb → master, with a single ReverbEffect.
        // The reverb bus is a classic send/return setup: the source
        // pushes into it via a send, the reverb bus emits wet-only,
        // and that joins the dry source path at master. To make the
        // wet-only contract explicit in v0.29.5+ (where the reverb
        // effect now has a dry passthrough), the bus's effect sets
        // dry to -100 dB — effectively muted — so the bus output is
        // unambiguously the wet field.
        cfg.busGraph.buses[1].id     = kBusReverb;
        cfg.busGraph.buses[1].parent = kBusMaster;
        cfg.busGraph.buses[1].outputGainDb = 0.0f;
        cfg.busGraph.buses[1].effects[0].kind             = EffectKind::Reverb;
        cfg.busGraph.buses[1].effects[0].reverbDecay      = 0.9f;
        cfg.busGraph.buses[1].effects[0].reverbHfDamping  = 0.3f;
        cfg.busGraph.buses[1].effects[0].reverbDryGainDb  = -100.0f;
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
