// audio_engine/config.h
//
// Configuration consumed by AudioRuntime::Initialize(). Values are read once
// and used to size all internal storage; they do not mutate after Initialize
// returns. Resizing is not supported.

#ifndef AUDIO_ENGINE_CONFIG_H
#define AUDIO_ENGINE_CONFIG_H

#include <cstdint>
#include "audio_engine/types.h"
#include "audio_engine/bus.h"

namespace audio {

// Per-player, per-category token-bucket budget for SubmitReplicatedEvent.
// `tokensPerSecond <= 0` means "unlimited" (no rate limit applied for
// this category). `burstCapacity` is the maximum tokens the bucket can
// hold; defaults to one second of refill if left at 0.
struct ReplicationRateLimitBudget {
    float    tokensPerSecond = 0.0f;
    uint32_t burstCapacity   = 0;
};

// Per-category replication rate-limit configuration. Indexed by
// AudioCategory (SFX, Voice, Music, Ambience, UI, Dialogue), so the
// array size must match AudioCategory::Count.
//
// The consultant-prescribed defaults below match plausible gameplay:
//   * SFX 50/sec/player: comfortably above any realistic rate of
//     gunshots, explosions, footsteps a single player can drive.
//   * Voice 150/sec/player: 50 Hz Opus produces 50 packets/sec/player;
//     3x headroom covers retransmits and bursty re-broadcast.
//   * Music 5/sec/player: music transitions are rare; 5 absorbs
//     legitimate state-flapping but rejects flooding.
//   * Dialogue 20/sec/player: NPC barks, taunts; bursty in combat.
//   * Ambience 10/sec/player: zone changes, weather; mostly host-driven.
//   * UI 0 (unlimited): UI sounds are usually local-only and not
//     replicated; left unlimited so legitimate replication isn't gated.
//
// To disable rate limiting entirely (e.g. for trusted-host single-machine
// testing), zero out every category's tokensPerSecond. The runtime treats
// 0 as "no limit" rather than "0 events/sec."
struct ReplicationRateLimitConfig {
    ReplicationRateLimitBudget perCategory[6] = {
        { 50.0f,  50 },   // SFX
        { 150.0f, 150 },  // Voice
        { 5.0f,   5  },   // Music
        { 10.0f,  10 },   // Ambience
        { 0.0f,   0  },   // UI (unlimited; UI is rarely replicated)
        { 20.0f,  20 }    // Dialogue
    };

    // Maximum number of distinct AudioPlayerIds tracked simultaneously.
    // When this many players are active and a new playerId arrives, the
    // least-recently-seen slot is recycled. Sized for typical session
    // capacity; bump for battle-royale-scale lobbies.
    uint32_t maxTrackedPlayers = 64;

    // PlayerId-cycling defense (anti-DoS).
    //
    // Without this cap, an attacker can flood SubmitReplicatedEvent or
    // OnVoicePacket using a different fake playerId for each packet:
    //   * Every event allocates a new slot in the LRU table, evicting
    //     legitimate players' counters and bucket state.
    //   * Per-player rate limiting becomes useless because the
    //     attacker never collides with their own bucket.
    //
    // This cap limits how many new (never-seen-before) playerIds may
    // be admitted into the rate limiter's slot table per simulation
    // tick. The counter resets on OnTickAdvanced. Once exceeded, all
    // subsequent events from new playerIds in the same tick are
    // rejected (counted in `Stats::replicationEventsRejectedNewIdBudget`).
    //
    // Sized for typical join cadence: 4-8 players showing up at a
    // session start, occasional reconnects mid-session. Bump for
    // tournament lobbies that admit dozens of players in one tick.
    // Set to 0 to disable the cap entirely (not recommended on
    // internet-facing hosts).
    uint32_t maxNewPlayersPerTick = 8;
};

struct AudioRuntimeBudget {
    uint32_t maxActiveEmitters         = 128;
    uint32_t maxSpatialEmitters        = 64;
    uint32_t maxVoiceSources           = 16;
    uint32_t maxOcclusionChecksPerFrame = 12;
    uint32_t maxStreamingAssets        = 32;
    uint32_t maxStreamingVoices        = 8;     // concurrent streaming voices
    uint32_t maxRegisteredSounds       = 256;
    uint32_t maxGameEventsPerFrame     = 256;
    uint32_t maxNetworkEventsPerFrame  = 256;
    // Interest-management cap on per-tick spatial processing. When > 0,
    // each tick the runtime sorts active emitters by distance to the
    // listener and only runs the spatializer + posts UpdateParams for
    // the closest N. Emitters outside the top-N are muted by posting a
    // single zero-gain UpdateParams the tick they fall out, and remain
    // silent until they re-enter the top-N (at which point a fresh
    // UpdateParams unmutes them). 0 (default) = unlimited, every active
    // emitter is processed every tick.
    //
    // Use this for shooters that have many more potential audible
    // sources than CPU budget allows (e.g. 60-player battle royales
    // where every player's footstep emitter exists but only 30 should
    // be heard at any moment).
    uint32_t maxActiveEmittersProcessedPerTick = 0;
};

struct AudioConfig {
    // Output
    uint32_t        sampleRate    = 48000;
    uint32_t        bufferSize    = 512;            // frames per backend callback
    AudioOutputMode outputMode    = AudioOutputMode::Stereo;

    // Subsystem toggles
    bool enableVoice            = true;
    bool enableOcclusion        = true;
    bool enableReverbZones      = true;
    bool enableHrtf             = false;
    bool enableDoppler          = true;
    bool enableAirAbsorption    = true;

    // Speed of sound in m/s used by the Doppler model. 343 is air at ~20 °C;
    // dial down for thin atmospheres or up for water (1480) if you want
    // more pronounced Doppler.
    float speedOfSound = 343.0f;

    // Air absorption: amount per meter contributed to the per-voice LPF's
    // lowPassAmount. With the default 1/250 = 0.004, a source 250 m away
    // saturates the air-absorption contribution at 1.0 (heavy muffle).
    // Combined with occlusion via max(occlusionAmount, airAmount) so the
    // strongest source wins; cascading both LPFs would over-damp.
    float airAbsorptionPerMeter = 1.0f / 250.0f;

    // Global reverb send level [0, 1]: how much of every spatialized voice's
    // signal is forwarded to the conventional reverb bus (kBusReverb) by
    // the default spatializer. Defaults to 0 so a runtime with no reverb
    // bus configured stays silent on the send path. The host opts in by:
    //   (1) adding a bus with id=kBusReverb (typically with a single
    //       EffectKind::Reverb effect on its chain),
    //   (2) bumping this knob above 0.
    float globalReverbSend = 0.0f;

    // Late-event discard horizon (ms). Events older than this when drained
    // from the queue are dropped rather than played. Applies to events; voice
    // packets have their own jitter-buffer policy and are independent.
    uint32_t lateEventDiscardMs = 250;

    // Voice
    uint32_t voicePacketRingDepth = 32;     // per sender
    uint32_t voicePcmRingFrames   = 9600;   // 200 ms at 48 kHz
    uint32_t voiceMaxPacketBytes  = 1500;
    uint32_t voiceJitterTargetMs  = 60;

    // Streaming
    uint32_t streamingRingFrames        = 24000;   // ~500 ms at 48 kHz, per voice
    uint32_t streamingDecodeChunkFrames = 2048;    // chunk size pumped per Update

    AudioRuntimeBudget budget;

    // Bus graph and DSP. When busGraph.busCount == 0, the runtime auto-builds
    // a single-bus topology with master only and no effects, and routes
    // every voice there. To use ducking / submixes / sidechain, define a
    // multi-bus graph here.
    BusGraphConfig busGraph;

    // Replication rate limiting. Per-player, per-category token-bucket
    // limits on SubmitReplicatedEvent. Defends against malicious or buggy
    // clients flooding the runtime with thousands of events per tick.
    //
    // The deterministic clock used for token refill is the most recent
    // serverTimeMs reported via OnTickAdvanced, so rate-limit decisions
    // reproduce across replays for a fixed input timeline.
    //
    // Defaults are sized for plausible gameplay; see ReplicationRateLimitConfig.
    ReplicationRateLimitConfig replicationRateLimit;

    // Maximum number of distinct global (RTPC) parameters the runtime
    // will store. SetGlobalParameter calls beyond this budget return
    // AudioResult::BudgetExceeded; the store is unchanged. Sized
    // generously for typical games (a few dozen parameters is normal);
    // bump for projects with very large parameter graphs.
    uint32_t maxGlobalParameters = 256;

    // Maximum number of sounds that can have a volume-RTPC binding
    // registered at once. SetSoundVolumeRtpc beyond this budget
    // returns AudioResult::BudgetExceeded; existing bindings stay.
    // Updating an existing binding (re-binding the same sound) never
    // exceeds the budget. Sized for typical projects (heartbeat,
    // ambient layers, music stingers — usually < 20 bindings); bump
    // for very large authored-RTPC catalogs.
    uint32_t maxSoundRtpcBindings = 256;
};

} // namespace audio

#endif // AUDIO_ENGINE_CONFIG_H
