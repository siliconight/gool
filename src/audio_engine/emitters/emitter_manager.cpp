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

// audio_engine/emitters/emitter_manager.cpp

#include "audio_engine/emitters/emitter_manager.h"

#include <algorithm>
#include <cmath>

namespace audio {

EmitterManager::EmitterManager(uint32_t maxActiveEmitters)
    : slots_(maxActiveEmitters) {}

Result<EmitterHandle> EmitterManager::Create(const EmitterDescriptor& desc, bool oneShot) {
    EmitterRecord rec;
    rec.descriptor    = desc;
    rec.position      = desc.position;
    rec.forward       = desc.forward;
    rec.velocity      = desc.velocity;
    rec.oneShot       = oneShot;
    rec.activeSoundId = desc.soundId;

    auto handle = slots_.Allocate(std::move(rec));
    if (!handle) return AudioResult::BudgetExceeded;
    if (auto* allocated = slots_.Get(*handle)) {
        // 1:1 mapping: mix slot index = emitter slot index. Voice sources use
        // a higher range; control thread caller is responsible for keeping
        // these from colliding (set via VoiceSourceManager.MixSlotBase()).
        allocated->assignedMixSlot = handle->index;
        // v0.78.0: stamp the monotonic sequence number AFTER the slot
        // is allocated, so only successful Creates consume a sequence.
        // Stamping here (rather than in AudioRuntimeImpl::CreateEmitter)
        // is critical: one-shots spawned by the event path go through
        // this Create() with oneShot=true and would otherwise miss the
        // stamp, breaking Oldest/Newest tie-breaking across lifecycles.
        allocated->createSequence = nextCreateSequence_++;
    }
    return *handle;
}

AudioResult EmitterManager::Destroy(EmitterHandle h) {
    if (!slots_.Free(h)) return AudioResult::InvalidHandle;
    return AudioResult::Success;
}

AudioResult EmitterManager::SetTransform(EmitterHandle h,
                                          const Vec3& pos,
                                          const Vec3& fwd,
                                          const Vec3& vel) {
    auto* rec = slots_.Get(h);
    if (!rec) return AudioResult::InvalidHandle;
    rec->position = pos;
    rec->forward  = fwd;
    rec->velocity = vel;
    return AudioResult::Success;
}

AudioResult EmitterManager::RecordReplicatedTransform(EmitterHandle h,
                                                        const Vec3&    pos,
                                                        const Vec3&    fwd,
                                                        const Vec3&    vel,
                                                        SimulationTick tick) {
    // Backward-compatible 5-arg form: chains through the mask
    // overload with All so pre-v0.18.0 behavior (every subfield
    // shifted on every call) is preserved.
    return RecordReplicatedTransform(h, TransformStateMask::All,
                                       pos, fwd, vel, tick);
}

AudioResult EmitterManager::RecordReplicatedTransform(EmitterHandle      h,
                                                        TransformStateMask mask,
                                                        const Vec3&        pos,
                                                        const Vec3&        fwd,
                                                        const Vec3&        vel,
                                                        SimulationTick     tick) {
    auto* rec = slots_.Get(h);
    if (!rec) return AudioResult::InvalidHandle;

    // Per-subfield shift-and-write. Unmasked subfields keep their
    // existing history; both samples in [0] and [1] are left
    // untouched. This is exactly the Tribes "state mask" semantics
    // applied to the subfields of a single object — components that
    // haven't changed don't waste a history slot or interpolation
    // cycle.
    if ((mask & TransformStateMask::Position) != 0) {
        rec->repPos[0] = rec->repPos[1];
        rec->repPos[1] = pos;
    }
    if ((mask & TransformStateMask::Forward) != 0) {
        rec->repFwd[0] = rec->repFwd[1];
        rec->repFwd[1] = fwd;
    }
    if ((mask & TransformStateMask::Velocity) != 0) {
        rec->repVel[0] = rec->repVel[1];
        rec->repVel[1] = vel;
    }

    // Tick advances unconditionally when at least one subfield is
    // fresh. The tick is the "I touched this emitter on this tick"
    // marker that the interpolator uses to know whether to apply
    // history at all; an update with zero subfields fresh wouldn't
    // reach here (the runtime's mask overload returns Success
    // without enqueuing when mask is None).
    rec->repTick[0] = rec->repTick[1];
    rec->repTick[1] = tick;
    return AudioResult::Success;
}

void EmitterManager::InterpolateReplicatedTransforms(float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    slots_.ForEach([&](EmitterHandle, EmitterRecord& rec) {
        if (!rec.descriptor.followsReplicatedTransform) return;

        // No history yet: keep current values.
        if (rec.repTick[1] == 0) return;

        if (rec.repTick[0] == 0) {
            // Single sample: snap to it.
            rec.position = rec.repPos[1];
            rec.forward  = rec.repFwd[1];
            rec.velocity = rec.repVel[1];
            return;
        }

        // alpha == 0 -> previous (one-tick lag); alpha == 1 -> latest;
        // anywhere in between is interpolated. Beyond `latest` (alpha would
        // exceed 1) we extrapolate using `velocity`. We cap alpha at 1
        // above; the host runtime is responsible for advancing alpha.
        const float a = alpha;
        rec.position.x = std::lerp(rec.repPos[0].x, rec.repPos[1].x, a);
        rec.position.y = std::lerp(rec.repPos[0].y, rec.repPos[1].y, a);
        rec.position.z = std::lerp(rec.repPos[0].z, rec.repPos[1].z, a);

        rec.forward.x  = std::lerp(rec.repFwd[0].x, rec.repFwd[1].x, a);
        rec.forward.y  = std::lerp(rec.repFwd[0].y, rec.repFwd[1].y, a);
        rec.forward.z  = std::lerp(rec.repFwd[0].z, rec.repFwd[1].z, a);

        rec.velocity   = rec.repVel[1];   // most-recent velocity for doppler
    });
}

void EmitterManager::BuildSnapshot(std::vector<SpatialEmitterView>& emitterViews,
                                     std::vector<Vec3>&               slotPositions,
                                     std::vector<uint8_t>&            slotOccupied) {
    const uint32_t cap = slots_.Capacity();
    if (emitterViews.size() != static_cast<size_t>(cap) + 1) {
        emitterViews.assign(static_cast<size_t>(cap) + 1, SpatialEmitterView{});
    }
    if (slotPositions.size() != static_cast<size_t>(cap) + 1) {
        slotPositions.assign(static_cast<size_t>(cap) + 1, Vec3{});
    }
    if (slotOccupied.size() != static_cast<size_t>(cap) + 1) {
        slotOccupied.assign(static_cast<size_t>(cap) + 1, 0u);
    }
    std::fill(slotOccupied.begin(), slotOccupied.end(), uint8_t{0});

    slots_.ForEach([&](EmitterHandle h, EmitterRecord& rec) {
        const uint32_t idx = h.index;
        SpatialEmitterView& v = emitterViews[idx];
        v.position        = rec.position;
        v.velocity        = rec.velocity;
        v.occlusionAmount = 0.0f;     // filled by occlusion system after this
        v.minDistance     = rec.descriptor.attenuation.minDistance;
        v.maxDistance     = rec.descriptor.attenuation.maxDistance;
        v.volumeFloor     = rec.descriptor.attenuation.volumeFloor;
        v.falloffModel    = rec.descriptor.attenuation.falloffModel;
        v.category        = rec.descriptor.category;
        v.spatialized     = rec.descriptor.isSpatialized;

        slotPositions[idx] = rec.position;
        slotOccupied[idx]  = 1u;
    });
}

} // namespace audio
