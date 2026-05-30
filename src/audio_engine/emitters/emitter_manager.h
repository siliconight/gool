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

// audio_engine/emitters/emitter_manager.h
//
// Owns the lifecycle of all playing emitters: long-lived host-managed
// emitters (CreateEmitter/DestroyEmitter) and engine-managed one-shots
// spawned by PlaySoundAtLocation events. Every active emitter has a slot.
//
// The slot is the source of truth (OOP-style, descriptor + replicated
// transform history + lifecycle flags). For per-frame spatializer work, the
// manager additionally maintains a SoA mirror; parallel arrays of just the
// fields the spatializer reads in its hot loop. The mirror is rebuilt at
// the start of each Update tick from the slot data.
//
// Replicated transforms keep two history slots so the control thread can
// linearly interpolate between (last - 1, last) with one tick of lag, and
// extrapolate from `last` using `velocity` for the trailing edge.

#ifndef AUDIO_ENGINE_EMITTERS_EMITTER_MANAGER_H
#define AUDIO_ENGINE_EMITTERS_EMITTER_MANAGER_H

#include <cstdint>
#include <vector>

#include "audio_engine/emitter.h"
#include "audio_engine/handles.h"
#include "audio_engine/result.h"
#include "audio_engine/spatializer.h"
#include "audio_engine/types.h"
#include "audio_engine/util/slot_map.h"

namespace audio {

struct EmitterRecord {
    EmitterDescriptor descriptor{};

    // Current authoritative transform (used by spatializer this tick).
    Vec3  position{};
    Vec3  forward{0.0f, 0.0f, -1.0f};
    Vec3  velocity{};

    // Replicated transform history for interpolation. Two ticks deep.
    // index 0 = previous, index 1 = latest. SimulationTick of 0 means unset.
    Vec3              repPos[2]{};
    Vec3              repFwd[2]{};
    Vec3              repVel[2]{};
    SimulationTick    repTick[2]{0, 0};

    // Smoothed parameters (gain/pitch/lowpass/reverb live in the SpatialParams
    // computed by the spatializer; user-defined parameters live here).
    float gainParam   = 1.0f;
    float pitchParam  = 1.0f;
    float lowPassParam = 0.0f;
    float reverbParam = 0.0f;

    // Lifecycle:
    //  - oneShot:  this slot was spawned by a PlaySound event; auto-destroy
    //              when the mixer reports completion (handled via timeout
    //              for now since the mixer doesn't surface completion yet).
    //  - assignedMixSlot: the global mix slot this emitter is bound to in
    //              the AudioMixer's voice array. Identical to the emitter
    //              slot index today (1:1 mapping under maxActiveEmitters).
    //  - mixerStarted: whether StartSound has been pushed to the mixer.
    bool       oneShot          = false;
    bool       mixerStarted     = false;
    uint32_t   assignedMixSlot  = 0;
    AudioSoundId activeSoundId  = kInvalidSoundId;

    // For one-shots: estimated frames remaining until the asset finishes,
    // tracked by control thread via wall-clock + asset frame count. When
    // it reaches 0, slot is freed.
    double oneShotFramesRemaining = 0.0;

    // Grace flag for the one-shot timing race: the first TickOneShots
    // pass after Create() does not decrement oneShotFramesRemaining,
    // guaranteeing the render thread gets at least one callback to
    // produce audio for the voice before the control thread can declare
    // it expired. Without this, a one-shot whose duration is shorter
    // than the Update tick (e.g. a 5 ms foley click on a 25 ms tick)
    // would be born and killed in the same tick: StartSound and Stop
    // would both arrive on the mixer's command ring before any render
    // pulled samples from the asset. The flag flips to true on the
    // first TickOneShots pass it sees.
    bool firstTickPassed = false;

    // Prediction id from the originating AudioEvent. Non-zero if the
    // host stamped one for client-side-predicted-event reconciliation.
    // The runtime scans emitters by this id to fade-out cancelled
    // predictions; the field becomes invalid when this slot is freed,
    // so no separate map cleanup is required.
    uint64_t predictionId = 0;

    // Interest-management mute state. True when this tick's spatial
    // pass deemed the emitter outside the closest-N budget; the runtime
    // posted a single zero-gain UpdateParams to silence it on the
    // mixer side, then skips re-issuing UpdateParams until the emitter
    // re-enters the top-N. Acts as an edge-trigger so we don't spam
    // mute-commands every tick at idle.
    bool inInterestMute = false;

    // v0.78.0: monotonically-increasing per-manager sequence number,
    // stamped at Create() time. Used by EvictionTieBreaker::{Oldest,
    // Newest} to disambiguate same-priority candidates in
    // TryEvictForPersistent. Frame-based tagging would tie within a
    // single Update tick (multiple Creates can land in the same tick);
    // wall-clock would break determinism. A monotonic sequence is
    // both tick-immune and replay-deterministic, and the stamp lives
    // on EmitterManager (not AudioRuntimeImpl) so one-shots spawned
    // by the event path receive a sequence number too — keeping
    // Oldest/Newest correct across both lifecycles. A value of 0
    // means "never stamped"; the first stamp is 1.
    uint64_t createSequence = 0;
};

class EmitterManager {
public:
    explicit EmitterManager(uint32_t maxActiveEmitters);

    Result<EmitterHandle> Create(const EmitterDescriptor& desc, bool oneShot = false);
    AudioResult           Destroy(EmitterHandle h);

    EmitterRecord*       Get(EmitterHandle h)       noexcept { return slots_.Get(h); }
    const EmitterRecord* Get(EmitterHandle h) const noexcept { return slots_.Get(h); }
    bool                 IsValid(EmitterHandle h) const noexcept { return slots_.IsValid(h); }

    AudioResult SetTransform(EmitterHandle h, const Vec3& pos, const Vec3& fwd, const Vec3& vel);

    AudioResult RecordReplicatedTransform(EmitterHandle h,
                                            const Vec3&    pos,
                                            const Vec3&    fwd,
                                            const Vec3&    vel,
                                            SimulationTick tick);

    // v0.18.0 Tier-A: mask-aware variant. Only the subfields whose
    // mask bits are set are shifted into history; the others retain
    // their previous values. The tick is always updated when any
    // subfield is fresh (the tick is the "this record was touched"
    // marker for the interpolator). Used by Phase 4 to honor the
    // host's TransformStateMask choice from
    // AudioRuntime::UpdateReplicatedTransform.
    AudioResult RecordReplicatedTransform(EmitterHandle      h,
                                            TransformStateMask mask,
                                            const Vec3&        pos,
                                            const Vec3&        fwd,
                                            const Vec3&        vel,
                                            SimulationTick     tick);

    // Apply tick interpolation to all replicated-following emitters.
    // serverTimeMs is the latest tick advancement timestamp; alpha is the
    // 0..1 progress between the last two ticks based on host clock.
    void InterpolateReplicatedTransforms(float alpha);

    // Build the SoA snapshot. slotPositions/slotOccupied are indexed by slot
    // index in [0, maxActiveEmitters], with index 0 always empty/unused.
    // emitterViews is parallel-indexed and only populated for occupied slots.
    // Using uint8_t for slotOccupied (not std::vector<bool>) so we can hand
    // a contiguous buffer to OcclusionSystem.
    void BuildSnapshot(std::vector<SpatialEmitterView>& emitterViews,
                       std::vector<Vec3>&               slotPositions,
                       std::vector<uint8_t>&            slotOccupied);

    // Decrement one-shot frame counters and free expired slots. Returns the
    // number of emitters freed this tick (callers use this to enqueue Stop
    // commands to the mixer).
    template <typename DestroyHook>
    uint32_t TickOneShots(double framesPerSecond, double deltaSeconds, DestroyHook&& onDestroy);

    uint32_t Count()    const noexcept { return slots_.Count(); }
    uint32_t Capacity() const noexcept { return slots_.Capacity(); }

    template <typename F>
    void ForEach(F&& fn) { slots_.ForEach(std::forward<F>(fn)); }

    template <typename F>
    void ForEach(F&& fn) const { slots_.ForEach(std::forward<F>(fn)); }

private:
    util::SlotMap<EmitterHandle, EmitterRecord> slots_;

    // v0.78.0: monotonic counter feeding EmitterRecord::createSequence.
    // Incremented and stamped inside Create() on success. Per-manager
    // (not per-runtime) so tests with multiple AudioRuntime instances
    // don't share sequence space. uint64_t never wraps for any
    // plausible game session length.
    uint64_t nextCreateSequence_ = 1;
};

template <typename DestroyHook>
uint32_t EmitterManager::TickOneShots(double framesPerSecond,
                                        double deltaSeconds,
                                        DestroyHook&& onDestroy) {
    const double framesThisTick = framesPerSecond * deltaSeconds;
    uint32_t freed = 0;

    // Collect handles to free (can't free during iteration).
    std::vector<EmitterHandle> toFree;
    slots_.ForEach([&](EmitterHandle h, EmitterRecord& rec) {
        if (!rec.oneShot) return;
        // Grace tick: a one-shot created earlier in the same Update gets
        // one tick of immunity from decrement so the render thread has
        // a chance to actually produce audio for it. Without this,
        // one-shots shorter than `framesThisTick` get killed before
        // they ever render.
        if (!rec.firstTickPassed) {
            rec.firstTickPassed = true;
            return;
        }
        rec.oneShotFramesRemaining -= framesThisTick;
        if (rec.oneShotFramesRemaining <= 0.0) {
            toFree.push_back(h);
        }
    });
    for (auto h : toFree) {
        if (auto* rec = slots_.Get(h)) {
            onDestroy(h, *rec);
        }
        slots_.Free(h);
        ++freed;
    }
    return freed;
}

} // namespace audio

#endif // AUDIO_ENGINE_EMITTERS_EMITTER_MANAGER_H
