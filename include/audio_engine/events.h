// audio_engine/events.h
//
// Tagged-union-style event POD submitted into the runtime's event ring(s).
// One concrete struct holds fields for every event type so the ring storage
// stays uniform and the hot path drains a fixed-size record. Fields not
// relevant for a given type are ignored.
//
// Two ingress paths exist:
//   * Game thread:    AudioRuntime::SubmitEvent
//   * Network thread: AudioRuntime::SubmitReplicatedEvent
// Both terminate in the same event handler running on the audio control
// thread inside Update().

#ifndef AUDIO_ENGINE_EVENTS_H
#define AUDIO_ENGINE_EVENTS_H

#include "audio_engine/types.h"
#include "audio_engine/handles.h"

namespace audio {

enum class AudioEventType : uint8_t {
    PlaySoundAtLocation,
    PlaySoundAttachedToActor,
    StopEmitter,
    SetEmitterParameter,
    SetEmitterTransform,
    RegisterVoiceSource,
    UnregisterVoiceSource,
    SetAudioZone,
    UpdateReplicatedTransform,
    TriggerSequence,
    SetGameState,
};

struct AudioEvent {
    AudioEventType         type              = AudioEventType::PlaySoundAtLocation;
    AudioSoundId           soundId           = kInvalidSoundId;
    AudioActorId           actorId           = kInvalidActorId;
    AudioPlayerId          playerId          = kInvalidPlayerId;
    EmitterHandle          emitter           = kNullEmitterHandle;
    Vec3                   position{};
    Vec3                   forward{};
    Vec3                   velocity{};
    AudioParameterId       parameterId       = 0;
    float                  parameterValue    = 0.0f;
    float                  parameterSmoothingMs = 0.0f;
    AudioSequenceId        sequenceId        = 0;
    AudioZoneId            zoneId            = kInvalidZoneId;
    SimulationTick         simulationTick    = 0;
    TimestampMs            timestampMs       = 0;
    AudioPriority          priority          = AudioPriority::Normal;
    AudioReplicationPolicy replicationPolicy = AudioReplicationPolicy::LocalOnly;

    // Category for per-player, per-category replication rate limiting on
    // the network thread. Defaults to SFX, which is the right choice for
    // gameplay-driven events (gunshots, footsteps, hits). Override for
    // music transitions, dialogue, ambience, or UI to apply that
    // category's bucket. See AudioConfig::replicationRateLimit.
    AudioCategory          category          = AudioCategory::SFX;

    // Per-event staleness override (item-driven). When > 0, the engine
    // drops this event during drain if (now - timestampMs) exceeds this
    // value, ignoring the global `AudioConfig::lateEventDiscardMs`. Use
    // 0 to fall back to the global default.
    //
    // The engine ships per-category suggestions in DefaultStalenessMsForCategory()
    // below: gunshots ~200 ms (any later and the visual has moved on),
    // voice events ~1000 ms (more network-tolerant), music transitions
    // ~5000 ms (still meaningful when delayed), UI never stales.
    uint32_t               maxStalenessMs    = 0;

    // Prediction ID for client-side-predicted local events. When non-zero,
    // the host can later call `AudioRuntime::CancelPredictedEvent(id, fadeMs)`
    // to fade out the resulting voice if server reconciliation rejects
    // the prediction. The engine maps this id to the emitter handle on
    // event handling and clears the mapping when the one-shot completes
    // naturally. 0 = not predicted; cancel is a no-op for such events.
    uint64_t               predictionId      = 0;

    // Convenience constructors. Defined out-of-line in events.cpp.
    static AudioEvent MakePlaySoundAtLocation(
        AudioSoundId sound,
        const Vec3& pos,
        AudioReplicationPolicy policy = AudioReplicationPolicy::LocalOnly,
        AudioPriority pri = AudioPriority::Normal,
        TimestampMs ts = 0);

    static AudioEvent MakePlaySoundAttachedToActor(
        AudioSoundId sound,
        AudioActorId actor,
        AudioReplicationPolicy policy = AudioReplicationPolicy::LocalOnly,
        AudioPriority pri = AudioPriority::Normal,
        TimestampMs ts = 0);

    static AudioEvent MakeStopEmitter(EmitterHandle h);

    static AudioEvent MakeSetEmitterParameter(
        EmitterHandle h,
        AudioParameterId param,
        float value,
        float smoothingMs = 50.0f);

    static AudioEvent MakeTriggerSequence(
        AudioSequenceId seq,
        AudioActorId actor = kInvalidActorId);
};

// Suggested staleness threshold per category. Hosts can use this when
// stamping `AudioEvent::maxStalenessMs` on outbound events; the engine
// itself does not auto-apply these (the field defaults to 0, meaning
// "fall back to global config"). The values are conservative defaults
// that match the perceptual expectations of each category — drop a
// gunshot from 250 ms ago because the visual has already moved on,
// but keep a music transition that's been queued for 3 seconds.
inline uint32_t DefaultStalenessMsForCategory(AudioCategory c) noexcept {
    switch (c) {
        case AudioCategory::SFX:      return 200;
        case AudioCategory::Voice:    return 1000;
        case AudioCategory::Music:    return 5000;
        case AudioCategory::Dialogue: return 2000;
        case AudioCategory::Ambience: return 1500;
        case AudioCategory::UI:       return 0;       // never stale
        default:                       return 0;
    }
}

} // namespace audio

#endif // AUDIO_ENGINE_EVENTS_H
