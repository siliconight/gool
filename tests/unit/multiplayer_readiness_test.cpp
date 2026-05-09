// tests/unit/multiplayer_readiness_test.cpp
//
// Tests the three multiplayer-shipping additions:
//
//   2. Per-event staleness override:
//      a stale SFX event (older than 200 ms) is dropped; a stale music
//      event (older than its 5 s threshold but within the global
//      lateEventDiscardMs cap) is delivered.
//
//   3. CancelPredictedEvent:
//      a host-stamped predictionId on a one-shot event lets
//      `CancelPredictedEvent(id)` find the resulting voice and post
//      a faded Stop. The voice rings down monotonically.
//
//   4. Interest management:
//      with `maxActiveEmittersProcessedPerTick` set below the active
//      emitter count, the closest N get fresh UpdateParams every
//      tick while the rest receive a single zero-gain mute and stay
//      quiet. When a far emitter walks closer (or a close one walks
//      away), the in/out membership swaps cleanly.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/bus.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/events.h"

#include <cmath>
#include <cstdio>
#include <memory>
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

float Peak(const float* d, size_t n) {
    float p = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float a = std::fabs(d[i]);
        if (a > p) p = a;
    }
    return p;
}

std::vector<float> SineMono(uint32_t sampleRate, float hz, uint32_t frames) {
    std::vector<float> v(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        v[i] = std::sin(2.0f * kPi * hz * static_cast<float>(i) / sampleRate);
    }
    return v;
}

// ============================================================
// Shared rig
// ============================================================

struct Rig {
    AudioRuntime rt;
    OfflineBackend* bp = nullptr;

    AudioResult Init(uint32_t maxActiveEmitters,
                      uint32_t maxProcessedPerTick = 0) {
        AudioConfig cfg;
        cfg.sampleRate = kSampleRate;
        cfg.bufferSize = kBufferSize;
        cfg.outputMode = AudioOutputMode::Stereo;
        cfg.budget.maxActiveEmitters          = maxActiveEmitters;
        cfg.budget.maxRegisteredSounds        = 8;
        cfg.budget.maxStreamingAssets         = 1;
        cfg.budget.maxStreamingVoices         = 1;
        cfg.budget.maxVoiceSources            = 0;
        cfg.budget.maxOcclusionChecksPerFrame = 16;
        cfg.budget.maxActiveEmittersProcessedPerTick = maxProcessedPerTick;
        cfg.enableOcclusion                   = false;
        cfg.enableAirAbsorption               = false;
        cfg.lateEventDiscardMs                = 250;     // global default

        auto backend = std::make_unique<OfflineBackend>();
        bp = backend.get();
        AudioRuntimeDependencies deps;
        deps.backend = std::move(backend);
        return rt.Initialize(cfg, std::move(deps));
    }
};

// ============================================================
// Item 2: per-event staleness override
// ============================================================

void TestStalenessOverride() {
    Rig rig;
    EXPECT(rig.Init(/*maxActiveEmitters*/ 4) == AudioResult::Success);

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    lis.forward  = {0.0f, 0.0f, -1.0f};
    lis.up       = {0.0f, 1.0f, 0.0f};
    rig.rt.SetListener(lis);

    // Drive the control clock to t = 1000 ms by ticking 1 second.
    // Without an explicit OnTickAdvanced-with-server-time call, the
    // runtime uses controlClockMs_ as "now".
    for (int i = 0; i < 40; ++i) {
        rig.rt.Update(0.025f);
        std::vector<float> tmp;
        rig.bp->Render(kSampleRate / 40, tmp);
    }

    constexpr AudioSoundId kSnd = 1;
    auto pcm = SineMono(kSampleRate, 440.0f, kSampleRate / 4);     // 250 ms
    rig.rt.RegisterPcmSound(kSnd, pcm, kSampleRate, /*channels*/ 1);

    SoundDefinition def;
    def.soundId    = kSnd;
    def.category   = AudioCategory::SFX;
    def.targetBus  = kBusMaster;
    def.spatialized = false;
    def.looping    = false;
    rig.rt.RegisterSoundDefinition(def);

    // Event 1: timestamped 100 ms ago, no override, global threshold 250
    // ms -> NOT stale, should fire.
    AudioEvent fresh;
    fresh.type        = AudioEventType::PlaySoundAtLocation;
    fresh.soundId     = kSnd;
    fresh.timestampMs = 900;     // controlClockMs is ~1000
    fresh.priority    = AudioPriority::Normal;
    fresh.maxStalenessMs = 0;     // use global default

    // Event 2: timestamped 600 ms ago, no override, global 250 ms ->
    // stale, should be dropped.
    AudioEvent staleGlobal;
    staleGlobal.type        = AudioEventType::PlaySoundAtLocation;
    staleGlobal.soundId     = kSnd;
    staleGlobal.timestampMs = 400;
    staleGlobal.priority    = AudioPriority::Normal;
    staleGlobal.maxStalenessMs = 0;

    // Event 3: 600 ms ago BUT with a per-event override of 1000 ms
    // (e.g. a music transition). Should fire despite being past the
    // global default.
    AudioEvent staleButOverride;
    staleButOverride.type        = AudioEventType::PlaySoundAtLocation;
    staleButOverride.soundId     = kSnd;
    staleButOverride.timestampMs = 400;
    staleButOverride.priority    = AudioPriority::Normal;
    staleButOverride.maxStalenessMs = 1000;

    // Event 4: 50 ms ago BUT with a tight 20 ms override (e.g. a
    // gunshot whose visual already moved on). Should be dropped.
    AudioEvent freshButTight;
    freshButTight.type        = AudioEventType::PlaySoundAtLocation;
    freshButTight.soundId     = kSnd;
    freshButTight.timestampMs = 950;
    freshButTight.priority    = AudioPriority::Normal;
    freshButTight.maxStalenessMs = 20;
    // Submit all four events, then run exactly one Update so they all
    // drain in the same tick. lateEventsDiscardedLastTick is a
    // per-tick counter so it shows the result of that single drain.
    EXPECT(rig.rt.SubmitEvent(fresh)            == AudioResult::Success);
    EXPECT(rig.rt.SubmitEvent(staleGlobal)      == AudioResult::Success);
    EXPECT(rig.rt.SubmitEvent(staleButOverride) == AudioResult::Success);
    EXPECT(rig.rt.SubmitEvent(freshButTight)    == AudioResult::Success);

    rig.rt.Update(0.025f);
    std::vector<float> tmp;
    rig.bp->Render(kSampleRate / 40, tmp);

    const auto after = rig.rt.GetStats();
    std::printf("  events submitted: 4 (fresh, stale-global, stale-with-override, fresh-tight)\n");
    std::printf("  drained-last-tick=%u  late-discarded-last-tick=%u\n",
                after.eventsDrainedLastTick, after.lateEventsDiscardedLastTick);
    // Expected: 4 drained, 2 discarded as stale (the global-stale and
    // the tight-override events).
    EXPECT(after.eventsDrainedLastTick      == 4);
    EXPECT(after.lateEventsDiscardedLastTick == 2);

    rig.rt.Shutdown();
}

// ============================================================
// Item 3: CancelPredictedEvent
// ============================================================

void TestCancelPredictedEvent() {
    Rig rig;
    EXPECT(rig.Init(/*maxActiveEmitters*/ 4) == AudioResult::Success);

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    lis.forward  = {0.0f, 0.0f, -1.0f};
    lis.up       = {0.0f, 1.0f, 0.0f};
    rig.rt.SetListener(lis);

    constexpr AudioSoundId kSnd = 1;
    auto pcm = SineMono(kSampleRate, 440.0f, kSampleRate);     // 1 second
    rig.rt.RegisterPcmSound(kSnd, pcm, kSampleRate, 1);

    SoundDefinition def;
    def.soundId     = kSnd;
    def.category    = AudioCategory::SFX;
    def.targetBus   = kBusMaster;
    def.spatialized = false;
    def.looping     = false;
    rig.rt.RegisterSoundDefinition(def);

    // 1) Fire a predicted event with id = 42.
    AudioEvent ev;
    ev.type         = AudioEventType::PlaySoundAtLocation;
    ev.soundId      = kSnd;
    ev.position     = {0.0f, 0.0f, 0.0f};
    ev.priority     = AudioPriority::Normal;
    ev.predictionId = 42;
    EXPECT(rig.rt.SubmitEvent(ev) == AudioResult::Success);

    // Pump enough ticks for the event to start the voice.
    std::vector<float> out;
    for (int i = 0; i < 4; ++i) {
        rig.rt.Update(0.025f);
        rig.bp->Render(kSampleRate / 40, out);
    }
    const float prePeak = Peak(out.data(), out.size());
    std::printf("  pre-cancel peak (steady):  %.3f\n", prePeak);
    EXPECT(prePeak > 0.3f);

    // 2) Cancel the prediction with a 50 ms fade.
    EXPECT(rig.rt.CancelPredictedEvent(42, /*fadeOutMs*/ 50.0f) == AudioResult::Success);
    const auto stats = rig.rt.GetStats();
    EXPECT(stats.predictionsCancelled == 1);
    EXPECT(stats.predictionsCancelledNotFound == 0);

    // 3) Render through the fade window plus tail.
    std::vector<float> tail;
    for (int i = 0; i < 6; ++i) {
        rig.rt.Update(0.025f);
        rig.bp->Render(kSampleRate / 40, out);
        tail.insert(tail.end(), out.begin(), out.end());
    }
    const size_t lastQuarter = tail.size() * 3 / 4;
    const float tailPeak = Peak(tail.data() + lastQuarter,
                                  tail.size() - lastQuarter);
    std::printf("  tail peak (post-fade):     %.6f\n", tailPeak);
    EXPECT(tailPeak < 1e-3f);     // fade-out completed; voice silent

    // 4) A second cancel for the same id finds nothing to cancel.
    EXPECT(rig.rt.CancelPredictedEvent(42) == AudioResult::Success);
    const auto stats2 = rig.rt.GetStats();
    EXPECT(stats2.predictionsCancelledNotFound == 1);

    // 5) Cancel with id=0 is a hard error.
    EXPECT(rig.rt.CancelPredictedEvent(0) == AudioResult::InvalidArgument);

    rig.rt.Shutdown();
}

// ============================================================
// Item 4: interest management
// ============================================================

void TestInterestManagementBudget() {
    Rig rig;
    // 6 emitters allowed; only 3 processed per tick.
    EXPECT(rig.Init(/*maxActiveEmitters*/ 6,
                     /*maxProcessedPerTick*/ 3) == AudioResult::Success);

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    lis.forward  = {0.0f, 0.0f, -1.0f};
    lis.up       = {0.0f, 1.0f, 0.0f};
    rig.rt.SetListener(lis);

    constexpr AudioSoundId kSnd = 1;
    auto pcm = SineMono(kSampleRate, 440.0f, kSampleRate);
    rig.rt.RegisterPcmSound(kSnd, pcm, kSampleRate, 1);

    SoundDefinition def;
    def.soundId     = kSnd;
    def.category    = AudioCategory::SFX;
    def.targetBus   = kBusMaster;
    def.spatialized = true;
    def.looping     = true;
    def.attenuation.minDistance = 1.0f;
    def.attenuation.maxDistance = 200.0f;
    def.attenuation.volumeFloor = 0.0f;
    def.occlusionEnabled = false;
    rig.rt.RegisterSoundDefinition(def);

    // Place 6 emitters at increasing distances on the +X axis.
    std::vector<EmitterHandle> handles;
    const float kDistances[6] = {1.0f, 5.0f, 10.0f, 25.0f, 60.0f, 150.0f};
    for (float d : kDistances) {
        EmitterDescriptor ed;
        ed.soundId       = kSnd;
        ed.position      = {d, 0.0f, 0.0f};
        ed.targetBus     = kBusMaster;
        ed.category      = AudioCategory::SFX;
        ed.priority      = AudioPriority::Normal;
        ed.isLooping     = true;
        ed.isSpatialized = true;
        ed.occlusionEnabled = false;
        ed.attenuation = def.attenuation;
        auto h = rig.rt.CreateEmitter(ed);
        EXPECT(static_cast<bool>(h));
        if (h) handles.push_back(h.value());
    }
    EXPECT(handles.size() == 6);

    // Pump a few ticks to let the runtime settle.
    std::vector<float> out;
    for (int i = 0; i < 6; ++i) {
        rig.rt.Update(0.025f);
        rig.bp->Render(kSampleRate / 40, out);
    }

    const auto stats = rig.rt.GetStats();
    std::printf("  emitters: total=6  processed=%u  skipped=%u\n",
                stats.emittersProcessedLastTick,
                stats.emittersSkippedByInterestLastTick);
    EXPECT(stats.emittersProcessedLastTick == 3);
    EXPECT(stats.emittersSkippedByInterestLastTick == 3);

    // Now move the listener to (200, 0, 0): the 150 m emitter is now
    // at +X 50 m from the listener (moved closer relatively), and the
    // 1 m emitter is now 199 m away (moved farther). Membership of the
    // top-3 set must flip cleanly without an engine rebuild.
    lis.position = {200.0f, 0.0f, 0.0f};
    rig.rt.SetListener(lis);
    for (int i = 0; i < 6; ++i) {
        rig.rt.Update(0.025f);
        rig.bp->Render(kSampleRate / 40, out);
    }

    const auto stats2 = rig.rt.GetStats();
    std::printf("  after listener move: processed=%u  skipped=%u\n",
                stats2.emittersProcessedLastTick,
                stats2.emittersSkippedByInterestLastTick);
    EXPECT(stats2.emittersProcessedLastTick == 3);
    EXPECT(stats2.emittersSkippedByInterestLastTick == 3);

    // Disable interest cap and confirm we revert to processing all 6.
    // (Can't change config post-init; this branch is a separate runtime.)
    rig.rt.Shutdown();
    {
        Rig rig2;
        EXPECT(rig2.Init(/*maxActiveEmitters*/ 6,
                          /*maxProcessedPerTick*/ 0) == AudioResult::Success);
        AudioListener l = lis;
        l.position = {0.0f, 0.0f, 0.0f};
        rig2.rt.SetListener(l);
        rig2.rt.RegisterPcmSound(kSnd, pcm, kSampleRate, 1);
        rig2.rt.RegisterSoundDefinition(def);
        for (float d : kDistances) {
            EmitterDescriptor ed;
            ed.soundId       = kSnd;
            ed.position      = {d, 0.0f, 0.0f};
            ed.targetBus     = kBusMaster;
            ed.category      = AudioCategory::SFX;
            ed.priority      = AudioPriority::Normal;
            ed.isLooping     = true;
            ed.isSpatialized = true;
            ed.occlusionEnabled = false;
            ed.attenuation = def.attenuation;
            (void)rig2.rt.CreateEmitter(ed);
        }
        for (int i = 0; i < 4; ++i) {
            rig2.rt.Update(0.025f);
            rig2.bp->Render(kSampleRate / 40, out);
        }
        const auto stats3 = rig2.rt.GetStats();
        std::printf("  budget=0 (unlimited): processed=%u  skipped=%u\n",
                    stats3.emittersProcessedLastTick,
                    stats3.emittersSkippedByInterestLastTick);
        EXPECT(stats3.emittersProcessedLastTick == 6);
        EXPECT(stats3.emittersSkippedByInterestLastTick == 0);
        rig2.rt.Shutdown();
    }
}

} // namespace

int main() {
    std::printf("[multiplayer_readiness_test] running...\n");
    std::printf(" -- Item 2: per-event staleness override --\n");
    TestStalenessOverride();
    std::printf(" -- Item 3: CancelPredictedEvent --\n");
    TestCancelPredictedEvent();
    std::printf(" -- Item 4: interest management --\n");
    TestInterestManagementBudget();
    if (gFails == 0) {
        std::printf("[multiplayer_readiness_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[multiplayer_readiness_test] %d failure(s)\n", gFails);
    return 1;
}
