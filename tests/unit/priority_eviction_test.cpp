// tests/unit/priority_eviction_test.cpp
//
// Exercises priority-aware eviction of one-shots. The test drives a real
// AudioRuntime (with NullAudioBackend) so the whole stack runs: events
// drained on the control thread, EmitterManager allocations bumping into
// the budget, the runtime's eviction policy stealing slots from
// lower-priority playing one-shots, persistent CreateEmitter handles
// staying immune.
//
// Capacity is configured tight (3 emitter slots) so we can exhaust the
// pool deterministically.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/events.h"
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

constexpr AudioSoundId kSnd = 1;

// One-frame silent backend; enough to drive Update() through its
// pipeline; we only inspect stats, not audio.
class CountingBackend final : public IAudioBackend {
public:
    AudioResult Start(const AudioBackendConfig& cfg, IAudioRenderCallback* cb) override {
        cfg_ = cfg;
        cb_  = cb;
        return AudioResult::Success;
    }
    void Stop() override { cb_ = nullptr; }
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "Counting"; }
private:
    AudioBackendConfig cfg_{};
    IAudioRenderCallback* cb_ = nullptr;
};

struct Fixture {
    AudioRuntime runtime;

    Fixture(uint32_t maxEmitters, uint32_t maxSounds = 8) {
        AudioConfig cfg;
        cfg.sampleRate = 48000;
        cfg.bufferSize = 256;
        cfg.outputMode = AudioOutputMode::Stereo;
        cfg.budget.maxActiveEmitters     = maxEmitters;
        cfg.budget.maxRegisteredSounds   = maxSounds;
        cfg.budget.maxStreamingAssets    = 4;
        cfg.budget.maxStreamingVoices    = 2;
        cfg.budget.maxVoiceSources       = 0;

        AudioRuntimeDependencies deps;
        deps.backend = std::make_unique<CountingBackend>();
        const auto rc = runtime.Initialize(cfg, std::move(deps));
        if (rc != AudioResult::Success) {
            std::fprintf(stderr, "Initialize failed: %d\n", static_cast<int>(rc));
            ++gFails;
        }

        // Tiny 100-frame mono buffer registered as a "long enough not to
        // expire mid-test" PCM sound. With the runtime's 25 ms ticks that's
        // ~ 2 ms of audio, but TickOneShots only reaps when its frame
        // counter hits zero; for our purposes the slot stays occupied
        // long enough across a single Update() to trigger eviction.
        std::vector<float> pcm(48000, 0.0f);     // 1 second
        runtime.RegisterPcmSound(kSnd, pcm, 48000, 1);

        // One-shot defaults: not looping, not spatialized (so position
        // doesn't change attenuation, but is still used by the eviction
        // tie-breaker).
        SoundDefinition def;
        def.soundId     = kSnd;
        def.category    = AudioCategory::SFX;
        def.targetBus   = kInvalidBusId;
        def.spatialized = false;
        def.looping     = false;
        runtime.RegisterSoundDefinition(def);

        AudioListener lis;
        lis.position = {0.0f, 0.0f, 0.0f};
        runtime.SetListener(lis);
    }

    void Submit(AudioPriority p, Vec3 pos) {
        AudioEvent e;
        e.type     = AudioEventType::PlaySoundAtLocation;
        e.soundId  = kSnd;
        e.position = pos;
        e.priority = p;
        runtime.SubmitEvent(e);
    }

    void Tick() { runtime.Update(0.025f); }

    AudioRuntime::Stats Stats() { return runtime.GetStats(); }
};

void TestNoContentionFitsCleanly() {
    Fixture f(/*maxEmitters*/ 3);
    f.Submit(AudioPriority::Normal, {1.0f, 0.0f, 0.0f});
    f.Submit(AudioPriority::Normal, {2.0f, 0.0f, 0.0f});
    f.Tick();

    auto s = f.Stats();
    EXPECT(s.activeEmitters == 2);
    EXPECT(s.oneShotEvictions == 0);
    EXPECT(s.oneShotsDroppedFullPool == 0);
}

void TestNewLowPriDroppedWhenAllHigh() {
    // 3 High-priority one-shots fill the pool, a 4th Normal sound arrives;
    // it must be dropped, no eviction.
    Fixture f(/*maxEmitters*/ 3);
    f.Submit(AudioPriority::High, {1.0f, 0.0f, 0.0f});
    f.Submit(AudioPriority::High, {2.0f, 0.0f, 0.0f});
    f.Submit(AudioPriority::High, {3.0f, 0.0f, 0.0f});
    f.Tick();
    EXPECT(f.Stats().activeEmitters == 3);

    f.Submit(AudioPriority::Normal, {0.5f, 0.0f, 0.0f});
    f.Tick();

    auto s = f.Stats();
    EXPECT(s.activeEmitters == 3);
    EXPECT(s.oneShotsDroppedFullPool == 1);
    EXPECT(s.oneShotEvictions == 0);
}

void TestNewHighPriEvictsLowPri() {
    // 3 Normal one-shots fill the pool, then a Critical sound arrives.
    // Eviction must steal one Normal slot.
    Fixture f(/*maxEmitters*/ 3);
    f.Submit(AudioPriority::Normal, {10.0f, 0.0f, 0.0f});
    f.Submit(AudioPriority::Normal, {11.0f, 0.0f, 0.0f});
    f.Submit(AudioPriority::Normal, {12.0f, 0.0f, 0.0f});
    f.Tick();
    EXPECT(f.Stats().activeEmitters == 3);

    f.Submit(AudioPriority::Critical, {0.0f, 0.0f, 0.0f});
    f.Tick();

    auto s = f.Stats();
    EXPECT(s.activeEmitters == 3);          // pool still saturated
    EXPECT(s.oneShotEvictions == 1);
    EXPECT(s.oneShotsDroppedFullPool == 0);
}

void TestDistanceBreaksPriorityTie() {
    // 3 Normal one-shots at progressively further distances; a 4th Normal
    // arrives close. Distance tie-breaker must pick the FARTHEST as victim.
    Fixture f(/*maxEmitters*/ 3);
    f.Submit(AudioPriority::Normal, { 5.0f, 0.0f, 0.0f});       // 5m
    f.Submit(AudioPriority::Normal, {10.0f, 0.0f, 0.0f});       // 10m
    f.Submit(AudioPriority::Normal, {50.0f, 0.0f, 0.0f});       // 50m   <-- target
    f.Tick();
    EXPECT(f.Stats().activeEmitters == 3);

    f.Submit(AudioPriority::Normal, {1.0f, 0.0f, 0.0f});         // 1m, beats 50m
    f.Tick();

    auto s = f.Stats();
    EXPECT(s.activeEmitters == 3);
    EXPECT(s.oneShotEvictions == 1);
    EXPECT(s.oneShotsDroppedFullPool == 0);
}

void TestSamePriorityAndCloserNotEvicted() {
    // 3 close-range Normals; a 4th Normal but FURTHER arrives. The far one
    // shouldn't beat any of them; must be dropped, not evict.
    Fixture f(/*maxEmitters*/ 3);
    f.Submit(AudioPriority::Normal, {1.0f, 0.0f, 0.0f});         // 1m
    f.Submit(AudioPriority::Normal, {2.0f, 0.0f, 0.0f});         // 2m
    f.Submit(AudioPriority::Normal, {3.0f, 0.0f, 0.0f});         // 3m
    f.Tick();
    EXPECT(f.Stats().activeEmitters == 3);

    f.Submit(AudioPriority::Normal, {30.0f, 0.0f, 0.0f});        // 30m
    f.Tick();

    auto s = f.Stats();
    EXPECT(s.activeEmitters == 3);
    EXPECT(s.oneShotEvictions == 0);
    EXPECT(s.oneShotsDroppedFullPool == 1);
}

void TestPersistentEmittersImmune() {
    // Two persistent emitters via CreateEmitter occupy slots. Then several
    // High-priority one-shots arrive; they should not evict the persistent
    // ones (those are host-owned). Only the remaining 1 free slot is
    // available for one-shots; subsequent arrivals get dropped.
    Fixture f(/*maxEmitters*/ 3);

    EmitterDescriptor pd;
    pd.soundId       = kSnd;
    pd.position      = {100.0f, 0.0f, 0.0f};
    pd.priority      = AudioPriority::Low;        // even Low priority; must still be immune
    pd.isLooping     = true;
    pd.isSpatialized = false;
    auto h1 = f.runtime.CreateEmitter(pd);
    EXPECT(static_cast<bool>(h1));
    auto h2 = f.runtime.CreateEmitter(pd);
    EXPECT(static_cast<bool>(h2));

    f.Tick();
    EXPECT(f.Stats().activeEmitters == 2);

    // Pool has 1 slot free. First Critical fits (placed far away).
    f.Submit(AudioPriority::Critical, {10.0f, 0.0f, 0.0f});
    f.Tick();
    EXPECT(f.Stats().activeEmitters == 3);

    // Pool full. Second Critical arrives CLOSER; must evict the first
    // Critical (distance breaks the priority tie). The persistent emitters
    // remain immune. After eviction the pool is still 3.
    f.Submit(AudioPriority::Critical, {0.0f, 0.0f, 0.0f});
    f.Tick();

    auto s = f.Stats();
    EXPECT(s.activeEmitters == 3);
    EXPECT(s.oneShotEvictions == 1);            // displaced the prior one-shot
    EXPECT(s.oneShotsDroppedFullPool == 0);

    // A Normal one-shot now should be dropped; the only eligible victim
    // is a Critical one-shot which beats Normal regardless of distance.
    f.Submit(AudioPriority::Normal, {0.0f, 0.0f, 0.0f});
    f.Tick();
    s = f.Stats();
    EXPECT(s.oneShotsDroppedFullPool == 1);
    EXPECT(s.oneShotEvictions == 1);

    if (h1) f.runtime.DestroyEmitter(h1.value());
    if (h2) f.runtime.DestroyEmitter(h2.value());
}

} // namespace

int main() {
    std::printf("[priority_eviction_test] running...\n");
    TestNoContentionFitsCleanly();
    TestNewLowPriDroppedWhenAllHigh();
    TestNewHighPriEvictsLowPri();
    TestDistanceBreaksPriorityTie();
    TestSamePriorityAndCloserNotEvicted();
    TestPersistentEmittersImmune();
    if (gFails == 0) {
        std::printf("[priority_eviction_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[priority_eviction_test] %d failure(s)\n", gFails);
    return 1;
}
