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

    Fixture(uint32_t maxEmitters,
            uint32_t maxSounds = 8,
            EvictionMode mode = EvictionMode::HardFail,
            EvictionTieBreaker tie = EvictionTieBreaker::Furthest) {
        AudioConfig cfg;
        cfg.sampleRate = 48000;
        cfg.bufferSize = 256;
        cfg.outputMode = AudioOutputMode::Stereo;
        cfg.budget.maxActiveEmitters     = maxEmitters;
        cfg.budget.maxRegisteredSounds   = maxSounds;
        cfg.budget.maxStreamingAssets    = 4;
        cfg.budget.maxStreamingVoices    = 2;
        cfg.budget.maxVoiceSources       = 0;
        cfg.budget.evictionMode          = mode;
        cfg.budget.evictionTieBreaker    = tie;

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

// ===========================================================================
// v0.78.0: EvictionTieBreaker tests for the persistent eviction path
// (TryEvictForPersistent). These exercise CreateEmitter under
// EvictionMode::Priority, where the tie-breaker decides which of several
// same-priority candidates loses its slot. The pool is sized to 3 so we can
// reach the saturation boundary deterministically.
//
// All six tests share the same shape:
//   1. Fill the pool with 3 same-priority persistent emitters
//   2. Attempt to create a 4th, higher-priority persistent emitter
//   3. Assert which of the original 3 was evicted via GetEmitterPriority,
//      which returns -1 for freed slots.
// ===========================================================================

// Helper: persistent emitter at a given position+priority. Returns the handle
// from CreateEmitter so the test can probe GetEmitterPriority after eviction.
static Result<EmitterHandle> CreatePersistent(AudioRuntime& rt,
                                                AudioPriority pri,
                                                Vec3          pos) {
    EmitterDescriptor d;
    d.soundId       = kSnd;
    d.position      = pos;
    d.priority      = pri;
    d.isLooping     = true;
    d.isSpatialized = false;  // position still used by Furthest, matching
                              // the one-shot path's behavior in the older
                              // TestDistanceBreaksPriorityTie above.
    return rt.CreateEmitter(d);
}

void TestPersistentEvictionTieBreaker_Furthest() {
    // Three Normal-priority emitters at 1m, 10m, 100m. Incoming Critical
    // should evict the 100m one (largest squared distance to listener).
    Fixture f(/*maxEmitters*/ 3, /*maxSounds*/ 8,
              EvictionMode::Priority, EvictionTieBreaker::Furthest);

    auto h1 = CreatePersistent(f.runtime, AudioPriority::Normal, {  1.0f, 0, 0});
    auto h2 = CreatePersistent(f.runtime, AudioPriority::Normal, { 10.0f, 0, 0});
    auto h3 = CreatePersistent(f.runtime, AudioPriority::Normal, {100.0f, 0, 0});
    EXPECT(static_cast<bool>(h1));
    EXPECT(static_cast<bool>(h2));
    EXPECT(static_cast<bool>(h3));

    auto h4 = CreatePersistent(f.runtime, AudioPriority::Critical, {0, 0, 0});
    EXPECT(static_cast<bool>(h4));

    // h3 (100m) should be evicted; h1 and h2 survive.
    EXPECT(f.runtime.GetEmitterPriority(h1.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.runtime.GetEmitterPriority(h2.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.runtime.GetEmitterPriority(h3.value()) == -1);
    EXPECT(f.runtime.GetEmitterPriority(h4.value()) ==
            static_cast<int32_t>(AudioPriority::Critical));

    EXPECT(f.Stats().emittersEvictedByPriority == 1);
}

void TestPersistentEvictionTieBreaker_Oldest() {
    // Three Normal emitters created in known order. With Oldest, the
    // incoming Critical should evict the FIRST one created (h1).
    Fixture f(/*maxEmitters*/ 3, /*maxSounds*/ 8,
              EvictionMode::Priority, EvictionTieBreaker::Oldest);

    auto h1 = CreatePersistent(f.runtime, AudioPriority::Normal, {1.0f, 0, 0});
    auto h2 = CreatePersistent(f.runtime, AudioPriority::Normal, {1.0f, 0, 0});
    auto h3 = CreatePersistent(f.runtime, AudioPriority::Normal, {1.0f, 0, 0});
    EXPECT(static_cast<bool>(h1));
    EXPECT(static_cast<bool>(h2));
    EXPECT(static_cast<bool>(h3));

    auto h4 = CreatePersistent(f.runtime, AudioPriority::Critical, {0, 0, 0});
    EXPECT(static_cast<bool>(h4));

    EXPECT(f.runtime.GetEmitterPriority(h1.value()) == -1);              // evicted
    EXPECT(f.runtime.GetEmitterPriority(h2.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.runtime.GetEmitterPriority(h3.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.Stats().emittersEvictedByPriority == 1);
}

void TestPersistentEvictionTieBreaker_Newest() {
    // Mirror of Oldest: Newest evicts h3 (created most recently).
    Fixture f(/*maxEmitters*/ 3, /*maxSounds*/ 8,
              EvictionMode::Priority, EvictionTieBreaker::Newest);

    auto h1 = CreatePersistent(f.runtime, AudioPriority::Normal, {1.0f, 0, 0});
    auto h2 = CreatePersistent(f.runtime, AudioPriority::Normal, {1.0f, 0, 0});
    auto h3 = CreatePersistent(f.runtime, AudioPriority::Normal, {1.0f, 0, 0});
    EXPECT(static_cast<bool>(h1));
    EXPECT(static_cast<bool>(h2));
    EXPECT(static_cast<bool>(h3));

    auto h4 = CreatePersistent(f.runtime, AudioPriority::Critical, {0, 0, 0});
    EXPECT(static_cast<bool>(h4));

    EXPECT(f.runtime.GetEmitterPriority(h1.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.runtime.GetEmitterPriority(h2.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.runtime.GetEmitterPriority(h3.value()) == -1);              // evicted
    EXPECT(f.Stats().emittersEvictedByPriority == 1);
}

void TestPersistentEvictionTieBreaker_SlotOrder_Deterministic() {
    // SlotOrder reproduces pre-0.78.0 behavior: first slot in ForEach order
    // (= lowest slot index, since SlotMap iterates by index). With sequential
    // Create calls the lowest-index slot is h1, so h1 is the victim.
    // Pinning this prevents accidental future churn of "what's the legacy
    // tie-break order?"
    Fixture f(/*maxEmitters*/ 3, /*maxSounds*/ 8,
              EvictionMode::Priority, EvictionTieBreaker::SlotOrder);

    auto h1 = CreatePersistent(f.runtime, AudioPriority::Normal, {99.0f, 0, 0});  // far
    auto h2 = CreatePersistent(f.runtime, AudioPriority::Normal, { 1.0f, 0, 0});  // close
    auto h3 = CreatePersistent(f.runtime, AudioPriority::Normal, {50.0f, 0, 0});  // mid
    EXPECT(static_cast<bool>(h1));
    EXPECT(static_cast<bool>(h2));
    EXPECT(static_cast<bool>(h3));

    auto h4 = CreatePersistent(f.runtime, AudioPriority::Critical, {0, 0, 0});
    EXPECT(static_cast<bool>(h4));

    // h1 (first allocated slot) is evicted regardless of distance or age.
    // Note: positions are deliberately varied so that Furthest would have
    // picked h1 here too (it's the furthest), but Newest/Oldest would not.
    // We don't separately distinguish those — the point of SlotOrder is the
    // strategy is iteration-order, period.
    EXPECT(f.runtime.GetEmitterPriority(h1.value()) == -1);              // evicted
    EXPECT(f.runtime.GetEmitterPriority(h2.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.runtime.GetEmitterPriority(h3.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.Stats().emittersEvictedByPriority == 1);
}

void TestPersistentEvictionTieBreaker_AcrossLifecycles() {
    // The strategy must apply uniformly across both lifecycles — one-shots
    // and persistent emitters — because createSequence is stamped on every
    // EmitterManager::Create() regardless of which path triggered it.
    //
    // Scenario: a Normal one-shot lands first, then a Normal persistent
    // emitter. Under Oldest, an incoming Critical persistent should evict
    // the one-shot (older sequence).
    Fixture f(/*maxEmitters*/ 3, /*maxSounds*/ 8,
              EvictionMode::Priority, EvictionTieBreaker::Oldest);

    // Submit a Normal one-shot first, then tick so it's installed.
    f.Submit(AudioPriority::Normal, {1.0f, 0, 0});
    f.Tick();
    EXPECT(f.Stats().activeEmitters == 1);

    // Now create two persistent emitters; they get sequences 2 and 3.
    // Note: activeEmitters in Stats() is only republished inside Update(),
    // so we don't assert on it between CreateEmitter calls — instead we
    // verify by handle validity below.
    auto hPersist1 = CreatePersistent(f.runtime, AudioPriority::Normal, {1.0f, 0, 0});
    auto hPersist2 = CreatePersistent(f.runtime, AudioPriority::Normal, {1.0f, 0, 0});
    EXPECT(static_cast<bool>(hPersist1));
    EXPECT(static_cast<bool>(hPersist2));

    // Incoming Critical persistent: under Oldest, the one-shot loses
    // (createSequence = 1, lowest in the pool). Both persistent emitters
    // survive. This proves the tie-breaker doesn't accidentally privilege
    // one lifecycle over the other.
    auto hCritical = CreatePersistent(f.runtime, AudioPriority::Critical, {0, 0, 0});
    EXPECT(static_cast<bool>(hCritical));

    EXPECT(f.runtime.GetEmitterPriority(hPersist1.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.runtime.GetEmitterPriority(hPersist2.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.Stats().emittersEvictedByPriority == 1);
}

void TestPersistentEvictionTieBreaker_DoesNotChangePriorityRule() {
    // The tie-breaker only chooses among same-priority candidates. A
    // strictly-lower-priority emitter must always be preferred as victim,
    // even if a higher-priority emitter is "more attractive" under the
    // tie-break key (e.g. further away under Furthest).
    //
    // Setup: one Low at 1m (close, but lowest priority) and two Normal
    // emitters at 100m and 99m (far away, higher priority). Incoming High.
    // Even though the Normals are furthest, the Low must lose its slot
    // because its priority is strictly lower.
    Fixture f(/*maxEmitters*/ 3, /*maxSounds*/ 8,
              EvictionMode::Priority, EvictionTieBreaker::Furthest);

    auto hLow    = CreatePersistent(f.runtime, AudioPriority::Low,    {  1.0f, 0, 0});
    auto hNorm1  = CreatePersistent(f.runtime, AudioPriority::Normal, {100.0f, 0, 0});
    auto hNorm2  = CreatePersistent(f.runtime, AudioPriority::Normal, { 99.0f, 0, 0});
    EXPECT(static_cast<bool>(hLow));
    EXPECT(static_cast<bool>(hNorm1));
    EXPECT(static_cast<bool>(hNorm2));

    auto hHigh = CreatePersistent(f.runtime, AudioPriority::High, {0, 0, 0});
    EXPECT(static_cast<bool>(hHigh));

    EXPECT(f.runtime.GetEmitterPriority(hLow.value())   == -1);          // evicted
    EXPECT(f.runtime.GetEmitterPriority(hNorm1.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.runtime.GetEmitterPriority(hNorm2.value()) ==
            static_cast<int32_t>(AudioPriority::Normal));
    EXPECT(f.Stats().emittersEvictedByPriority == 1);
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
    // v0.78.0: tie-breaker tests
    TestPersistentEvictionTieBreaker_Furthest();
    TestPersistentEvictionTieBreaker_Oldest();
    TestPersistentEvictionTieBreaker_Newest();
    TestPersistentEvictionTieBreaker_SlotOrder_Deterministic();
    TestPersistentEvictionTieBreaker_AcrossLifecycles();
    TestPersistentEvictionTieBreaker_DoesNotChangePriorityRule();
    if (gFails == 0) {
        std::printf("[priority_eviction_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[priority_eviction_test] %d failure(s)\n", gFails);
    return 1;
}
