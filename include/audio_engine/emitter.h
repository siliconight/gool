// audio_engine/emitter.h
//
// Public emitter and sound-definition types. The internal Emitter object
// (in src/audio_engine/emitters/) holds the lifecycle state; this header
// defines what the host hands in.

#ifndef AUDIO_ENGINE_EMITTER_H
#define AUDIO_ENGINE_EMITTER_H

#include "audio_engine/types.h"
#include "audio_engine/handles.h"
#include "audio_engine/attenuation.h"
#include "audio_engine/bus.h"

namespace audio {

// Used at registration time. AudioRuntime::CreateEmitter copies these fields
// into its internal slot.
struct EmitterDescriptor {
    AudioSoundId           soundId           = kInvalidSoundId;
    AudioActorId           ownerActorId      = kInvalidActorId;
    Vec3                   position{};
    Vec3                   forward{0.0f, 0.0f, -1.0f};
    Vec3                   velocity{};
    AttenuationSettings    attenuation{};
    AudioPriority          priority          = AudioPriority::Normal;
    AudioReplicationPolicy replicationPolicy = AudioReplicationPolicy::LocalOnly;
    AudioCategory          category          = AudioCategory::SFX;
    // Explicit bus override. kInvalidBusId means "use the AudioConfig
    // category-to-bus map to resolve at CreateEmitter time."
    BusId                  targetBus         = kInvalidBusId;
    bool                   isLooping         = false;
    bool                   isSpatialized     = true;
    bool                   occlusionEnabled  = true;
    // Used by state-based emitters that follow replicated transforms; ignored
    // for purely local emitters.
    bool                   followsReplicatedTransform = false;

    // Fade-in duration in milliseconds. When > 0, the voice's gain
    // ramps from 0 to its computed target over this duration using
    // an equal-power (sine) curve, paired with the cosine fade-out
    // applied by DestroyEmitter / StopMixer so a crossfade between
    // two emitters has constant total power. Default 0 = no fade-in.
    float                  fadeInMs                   = 0.0f;

    // v0.19.0 Tier-B: replication priority for the host's network
    // thread. When `UpdateReplicatedTransform` is called against
    // this emitter, the runtime reads this value from an atomic
    // shadow array (written by CreateEmitter, indexed by slot)
    // and consults it under ring pressure: if the netTransforms_
    // ring is above 75% capacity, updates for emitters with
    // priority below 128 are dropped before enqueue and counted
    // in `Stats::transformsDroppedByPriority`. The Tribes paper's
    // Ghost Manager applies the same idea — when bandwidth is
    // tight, the highest-priority dirty state goes first.
    //
    // Range: 0..255. The middle band (128) is the default — under
    // pressure, defaults survive. Use higher (192–255) for
    // emitters whose position is critical to gameplay feel
    // (player gunshots, footsteps near the listener); use lower
    // (0–127) for ambient, far-distance, or peripheral effects
    // that can lose a few frames of tracking without complaint.
    //
    // The priority is per-emitter, not per-update. Hosts that want
    // per-update granularity can sidestep this by destroying and
    // recreating the emitter, or by waiting for v0.20.0's tier-C
    // work which exposes a per-update override.
    uint8_t                replicationPriority        = 128;
};

// Static, per-sound metadata. Registered once via
// AudioRuntime::RegisterSoundDefinition(); resolved into emitter defaults at
// CreateEmitter time.
struct SoundDefinition {
    AudioSoundId           soundId           = kInvalidSoundId;
    AudioCategory          category          = AudioCategory::SFX;
    AudioPriority          priority          = AudioPriority::Normal;
    AttenuationSettings    attenuation{};
    BusId                  targetBus         = kInvalidBusId;
    bool                   spatialized       = true;
    bool                   looping           = false;
    bool                   occlusionEnabled  = true;
    AudioReplicationPolicy defaultReplicationPolicy = AudioReplicationPolicy::LocalOnly;

    // Crossfade duration applied at the loop boundary for looping
    // sounds. Defaults to 0 = no crossfade, the cursor wraps as
    // fmod(cursor, length) and any discontinuity between the first
    // and last samples produces an audible click each iteration.
    // When > 0, the last `loopCrossfadeMs` of the buffer are blended
    // into the first `loopCrossfadeMs` using equal-power curves
    // (cos for the tail, sin for the head); the cursor then wraps
    // to `loopCrossfadeMs` rather than 0 so the head samples already
    // mixed in during the crossfade aren't replayed.
    //
    // Typical values: 5-20 ms for SFX loops, 50-200 ms for music
    // beds. The crossfade region must be less than half the buffer
    // duration; the engine clamps to that limit and falls back to
    // no-crossfade behavior if violated.
    float                  loopCrossfadeMs   = 0.0f;
};

} // namespace audio

#endif // AUDIO_ENGINE_EMITTER_H
