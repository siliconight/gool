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
