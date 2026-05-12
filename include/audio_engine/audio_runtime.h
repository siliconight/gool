// audio_engine/audio_runtime.h
//
// Top-level public API. The AudioRuntime is concrete (not an interface);
// dependency injection happens through AudioRuntimeDependencies for the four
// real polymorphism seams (backend, spatializer, geometry query, voice codec).
// Anything left null falls back to the engine's internal default
// implementation.
//
// Thread-role annotations document which methods are callable from which
// thread. Under Clang -Wthread-safety, calls that would violate the contract
// are flagged at compile time.

#ifndef AUDIO_ENGINE_AUDIO_RUNTIME_H
#define AUDIO_ENGINE_AUDIO_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "audio_engine/audio_file_format.h"
#include "audio_engine/export.h"
#include "audio_engine/types.h"
#include "audio_engine/handles.h"
#include "audio_engine/result.h"
#include "audio_engine/config.h"
#include "audio_engine/events.h"
#include "audio_engine/listener.h"
#include "audio_engine/emitter.h"
#include "audio_engine/backend.h"
#include "audio_engine/spatializer.h"
#include "audio_engine/geometry_query.h"
#include "audio_engine/voice_codec.h"
#include "audio_engine/thread_annotations.h"

namespace audio {

// Forward declaration for pimpl. Defined in src/audio_engine/runtime/.
class AudioRuntimeImpl;

// Forward declare to avoid pulling telemetry.h into every translation
// unit that includes audio_runtime.h. The host opts in by including
// audio_engine/telemetry.h where it constructs the sink.
class IRuntimeTelemetrySink;

// Same pattern for the event-level log sink (Phase 4.8).
class IRuntimeLogSink;

// Optional dependency injection. Any null pointer is replaced with the
// engine's built-in stub (NullAudioBackend, DistancePanSpatializer,
// NullGeometryQuery, StubVoiceCodec). The runtime takes ownership of any
// non-null pointers for the lifetime between Initialize() and Shutdown().
struct AudioRuntimeDependencies {
    std::unique_ptr<IAudioBackend>       backend;
    std::unique_ptr<ISpatializer>        spatializer;
    std::unique_ptr<IAudioGeometryQuery> geometryQuery;
    std::unique_ptr<IVoiceCodec>         voiceCodec;

    // Optional telemetry sink. When non-null and
    // AudioConfig::telemetryIntervalMs > 0, the runtime calls into
    // OnRuntimeStats() every N ms from Update(). Lifetime is the
    // host's responsibility — the runtime stores the pointer
    // verbatim and never deletes it. This is a raw pointer
    // intentionally: typical usage has the sink outlive the runtime
    // (e.g. Prometheus sink lives as long as the HTTP server) and
    // ownership doesn't transfer.
    IRuntimeTelemetrySink*               telemetrySink = nullptr;

    // Optional event-level log sink. When non-null, the runtime calls
    // into OnLogEvent() at the listed hook points (late-event
    // discard, RTPC budget exceeded, replication validator
    // rejection, mixer-underrun delta, ...). The runtime serializes
    // calls via an internal mutex so sinks don't need to be
    // thread-safe themselves. Lifetime is host-managed; the runtime
    // never deletes the pointer. Pass nullptr (default) to disable
    // logging entirely; ShouldLog fast-paths via the nullptr check
    // before any field-array construction.
    IRuntimeLogSink*                     logSink       = nullptr;
};

class AUDIO_ENGINE_EXPORT AudioRuntime {
public:
    AudioRuntime();
    ~AudioRuntime();

    AudioRuntime(const AudioRuntime&) = delete;
    AudioRuntime& operator=(const AudioRuntime&) = delete;
    AudioRuntime(AudioRuntime&&) = delete;
    AudioRuntime& operator=(AudioRuntime&&) = delete;

    // ---- Lifecycle (game thread) -----------------------------------------

    AudioResult Initialize(const AudioConfig&        config,
                            AudioRuntimeDependencies deps = {})
        AUDIO_REQUIRES(GameThread);

    void Shutdown() AUDIO_REQUIRES(GameThread);

    bool IsInitialized() const noexcept;

    // Per-frame tick. Drains event queues, advances simulation, recomputes
    // spatial state, publishes the next mixer snapshot for the render thread.
    // Called on the audio control thread (often the game thread for simple
    // single-threaded host integrations).
    void Update(float deltaSeconds) AUDIO_REQUIRES(ControlThread);

    // ---- Sound registry (game thread) ------------------------------------

    AudioResult RegisterSoundDefinition(const SoundDefinition& def)
        AUDIO_REQUIRES(GameThread);

    // Pre-loads a PCM-decoded asset and pins it for the lifetime of the
    // runtime. `samples` is interleaved float32. `sampleRate` must match
    // the engine's configured output rate; resampling, when needed, is
    // the caller's responsibility (the file-decoder paths handle this
    // themselves via ResamplingDecoder).
    AudioResult RegisterPcmSound(AudioSoundId            id,
                                  std::span<const float>  samples,
                                  uint32_t                sampleRate,
                                  uint32_t                channels)
        AUDIO_REQUIRES(GameThread);

    // ---- Decoded-file registration (game thread) -------------------------
    // These open a file, decode the entire payload, resample to the engine's
    // sample rate, and pin the result like RegisterPcmSound. Suitable for
    // short SFX (sub-second). Long files block the game thread during
    // decode; use RegisterStreamingSoundFromFile for music and dialog
    // instead, or call this during a loading screen.
    //
    // Format detection: extension first (.wav/.ogg/.oga/.flac), magic-byte
    // sniff as fallback. Multi-channel (>2) is downmixed to stereo.
    //
    // Decoders are conditionally compiled. When AUDIO_ENGINE_DECODERS_WAV /
    // _OGG / _FLAC is OFF, the corresponding format returns
    // AudioResult::Unsupported.

    AudioResult RegisterSoundFromFile(AudioSoundId id, const char* path)
        AUDIO_REQUIRES(GameThread);

    AudioResult RegisterSoundFromMemory(AudioSoundId             id,
                                         std::span<const uint8_t> bytes,
                                         AudioFileFormat          formatHint = AudioFileFormat::Auto)
        AUDIO_REQUIRES(GameThread);

    // Registers a streaming sound. Decode happens incrementally on the
    // control thread during Update(); the mixer reads through an SPSC float
    // ring per playing voice. The file handle is held open for the lifetime
    // of the registration.
    //
    // Streaming voices share a pool sized by config.budget.maxStreamingVoices
    // (default 8). Starting more than that many concurrent streaming voices
    // returns AudioResult::BudgetExceeded on the StartSound path.

    AudioResult RegisterStreamingSoundFromFile(AudioSoundId id, const char* path)
        AUDIO_REQUIRES(GameThread);

    // Register a streaming sound from a caller-supplied PCM float buffer.
    // The runtime takes ownership of `samples` (move) and wraps it in a
    // synthetic decoder. Useful for procedurally-generated music or for
    // exercising the streaming pipeline without a file decoder. Resampling
    // to the engine rate happens automatically.
    AudioResult RegisterStreamingSoundFromMemory(AudioSoundId         id,
                                                  std::vector<float>&& samples,
                                                  uint32_t             sampleRate,
                                                  uint32_t             channels)
        AUDIO_REQUIRES(GameThread);

    // ---- Listener (game thread) ------------------------------------------

    void SetListener(const AudioListener& listener) AUDIO_REQUIRES(GameThread);

    // ---- Emitters (game thread) ------------------------------------------

    Result<EmitterHandle> CreateEmitter(const EmitterDescriptor& desc)
        AUDIO_REQUIRES(GameThread);

    AudioResult DestroyEmitter(EmitterHandle handle,
                                 float         fadeOutMs = 0.0f)
        AUDIO_REQUIRES(GameThread);

    AudioResult SetEmitterTransform(EmitterHandle handle,
                                     const Vec3&  position,
                                     const Vec3&  forward,
                                     const Vec3&  velocity)
        AUDIO_REQUIRES(GameThread);

    AudioResult SetEmitterParameter(EmitterHandle    handle,
                                     AudioParameterId paramId,
                                     float            value,
                                     float            smoothingMs = 50.0f)
        AUDIO_REQUIRES(GameThread);

    // Convenience: set the playback speed of an emitter. speed == 1.0
    // is normal speed, 2.0 plays at double speed (one octave up),
    // 0.5 plays at half speed (one octave down). Pitch and speed are
    // coupled (tape-style) — pitch-independent time-stretching is not
    // currently supported. Wraps SetEmitterParameter with the Pitch
    // parameter id; the runtime smooths the change over `smoothingMs`
    // milliseconds (default 50 ms) so abrupt changes don't click.
    inline AudioResult SetEmitterPlaybackSpeed(EmitterHandle handle,
                                                  float         speed,
                                                  float         smoothingMs = 50.0f) {
        return SetEmitterParameter(handle, AudioParameterIds::Pitch,
                                    speed, smoothingMs);
    }

    // ---- Global (RTPC) parameters (game thread) --------------------------
    // A flat name → float store for "real-time parameter control" values
    // that authored sound definitions can reference. Common examples:
    // "health", "wetness", "fatigue". The store is host-facing only at
    // this stage — values are recorded and surfaced via GetGlobalParameter
    // for game-side polling and debug overlays. Render-thread modulation
    // (sound definitions reading global parameters and adjusting volume
    // / cutoff / pitch automatically) is a future feature; this API ships
    // the storage and observability now so host code can build against it
    // and the upgrade path is additive.
    //
    // Lookups are O(1) average via an internal hash map. The store is
    // bounded — see AudioConfig::maxGlobalParameters (default 256). A
    // SetGlobalParameter call that would exceed the budget returns
    // AudioResult::BudgetExceeded and leaves the store unchanged.
    //
    // Parameter IDs: use HashParameterName("name") for string-keyed
    // parameters (the GDScript `Gool.set_rtpc("health", 0.3)` facade
    // does this internally). Or pass any AudioParameterId your host
    // code reserves above AudioParameterIds::HostBase.
    AudioResult SetGlobalParameter(AudioParameterId paramId, float value)
        AUDIO_REQUIRES(GameThread);

    // Read the current value. Returns true if the parameter has been
    // set at least once; false if it has never been set (in which case
    // `outValue` is left untouched). Cheap; safe to call every frame.
    bool GetGlobalParameter(AudioParameterId paramId,
                             float&           outValue) const
        AUDIO_REQUIRES(GameThread);

    // Remove a parameter from the store. Returns true if it existed.
    // Idempotent: clearing an unset parameter returns false but is
    // not an error.
    bool ClearGlobalParameter(AudioParameterId paramId)
        AUDIO_REQUIRES(GameThread);

    // Number of parameters currently stored. Useful for budget checks
    // and debug overlays.
    size_t GetGlobalParameterCount() const AUDIO_REQUIRES(GameThread);

    // ---- RTPC modulation (game thread) -----------------------------------
    //
    // Bind a sound's parameter target to a global RTPC. Each Update tick
    // the runtime walks active emitters, looks up bindings for each
    // emitter's `soundId`, reads each binding's parameter value from
    // the global parameter store, applies the configured curve and
    // input/output remapping, and pushes the result through the parameter
    // smoother as the per-voice target on the appropriate parameter.
    //
    // Skip-when-unset semantics: if a binding's parameter has never been
    // set via SetGlobalParameter, that binding is skipped this tick.
    // Other bindings on the same sound still evaluate. Authored
    // behavior is preserved for the unset case so binding-installation
    // order is gameplay-state-independent.
    //
    // Constraint: at most one binding per (soundId, target) pair. Setting
    // a second binding with the same target replaces the first. Use
    // ClearSoundRtpc(soundId, target) to remove just one target;
    // ClearAllSoundRtpc(soundId) removes every binding for the sound.
    //
    // Combiner semantics for the four targets:
    //   * Volume:        sp.gain *= rtpcOutput        (multiplicative)
    //   * Pitch:         sp.pitch *= rtpcOutput       (multiplicative)
    //   * LowPassCutoff: sp.lowPass = max(spatial, rtpcOutput)
    //                                                 (RTPC adds filter, never subtracts)
    //   * ReverbSend:    sp.reverbSend = clamp(spatial + rtpcOutput, 0, 1)
    //                                                 (RTPC adds wetness)
    //
    // Returns Success on bind, BudgetExceeded if the per-sound binding
    // table is full (kMaxBindingsPerSound = one per RtpcTarget value).
    AudioResult SetSoundRtpc(AudioSoundId             soundId,
                              const SoundRtpcBinding&  binding)
        AUDIO_REQUIRES(GameThread);

    // Remove a single binding for (soundId, target). Returns true if it
    // existed. Voices currently playing keep their last computed
    // smoothed value for that target (no snap-back to authored level).
    bool ClearSoundRtpc(AudioSoundId soundId, RtpcTarget target)
        AUDIO_REQUIRES(GameThread);

    // Remove every binding for a sound. Returns the number of bindings
    // removed. Idempotent: 0 when no bindings existed.
    size_t ClearAllSoundRtpc(AudioSoundId soundId)
        AUDIO_REQUIRES(GameThread);

    // Total number of registered bindings across all sounds. For budget
    // checks and debug overlays.
    size_t GetSoundRtpcBindingCount() const AUDIO_REQUIRES(GameThread);

    // ---- Bus graph (game thread) -----------------------------------------
    // The bus graph is defined in AudioConfig and immutable after Initialize.
    // These methods update parameter values on existing buses and effects.

    // Resolve a bus by its `debugName` (set via BusConfig::debugName at
    // build time, including by the JSON loader's `name` field). Returns
    // `kInvalidBusId` if no bus with that name exists. Use this to bridge
    // the gap between code that knows bus names (config files, hosts)
    // and code that needs BusId tokens (per-sound targeting,
    // SetBusGainDb, SetEffectParameter). O(N) over kMaxBuses; intended
    // for init-time and registration-time use, not per-frame.
    BusId FindBusIdByName(std::string_view name) const
        AUDIO_REQUIRES(GameThread);

    // Sets the output gain of a bus in dB. Smoothed internally by the
    // gain-applying stage on the render thread (~5 ms ramp); abrupt changes
    // do not click.
    AudioResult SetBusGainDb(BusId busId, float gainDb)
        AUDIO_REQUIRES(GameThread);

    // Sets a parameter on the effect at `effectIndex` within `busId`'s
    // effect chain. paramId is one of EffectParameter::*. Effects that do
    // not recognize the parameter ignore it on the render thread.
    AudioResult SetEffectParameter(BusId busId,
                                    uint32_t effectIndex,
                                    uint16_t paramId,
                                    float value)
        AUDIO_REQUIRES(GameThread);

    // ---- Events (game thread) --------------------------------------------

    AudioResult SubmitEvent(const AudioEvent& event) AUDIO_REQUIRES(GameThread);

    // Cancel a predicted local event by its `predictionId`. If a one-shot
    // emitter stamped with this id is still alive, post a faded Stop and
    // mark the slot for retire; otherwise no-op (the prediction either
    // already finished, was never produced, or was evicted).
    //
    // The host calls this when server reconciliation rejects a predicted
    // local action (e.g. "you didn't actually have ammo for that shot")
    // so the audio doesn't linger after the visual is unwound. Default
    // 50 ms fade is short enough to feel like a quick correction rather
    // than a fade-to-silence; pass a longer value for slow tail-outs.
    //
    // Returns InvalidArgument if predictionId is 0; Success otherwise
    // (Success even when no matching emitter is found, since "nothing to
    // cancel" is the same outcome as "successfully cancelled" from the
    // host's perspective).
    AudioResult CancelPredictedEvent(uint64_t predictionId, float fadeOutMs = 50.0f)
        AUDIO_REQUIRES(GameThread);

    // ---- Network seam (network thread) -----------------------------------
    // The four entry points exposed at the network seam, one per runtime
    // model:
    //   * tick advancement      -> OnTickAdvanced
    //   * remote audio events   -> SubmitReplicatedEvent
    //   * replicated state      -> UpdateReplicatedTransform
    //   * voice packets         -> OnVoicePacket
    // Voice does not wait on tick. State-based emitters are not re-fired
    // as events. Late-event discard applies to events and state but not to
    // voice (which has its own jitter buffer policy).

    void OnTickAdvanced(SimulationTick tick, TimestampMs serverTimeMs)
        AUDIO_REQUIRES(NetworkThread);

    // 1-arg form: legacy / convenience. Treats source as Unknown,
    // permissive policy enforcement. Suitable for tests and for hosts
    // that don't yet distinguish server-authored from client-forwarded
    // events.
    AudioResult SubmitReplicatedEvent(const AudioEvent& event)
        AUDIO_REQUIRES(NetworkThread);

    // 2-arg form: explicit trust label.
    //
    // Use `ReplicationSource::Server` when the event was authored by
    // your authoritative server logic — the runtime trusts the
    // replicationPolicy field verbatim.
    //
    // Use `ReplicationSource::Client` when forwarding an event that
    // arrived from a network peer — the runtime rejects events whose
    // declared `replicationPolicy` is `ServerAuthoritative` (clients
    // cannot author server-authoritative state changes), counted in
    // `Stats::replicationEventsRejectedByValidator` (returned as
    // `AudioResult::PolicyViolation`).
    //
    // The validator hook (if installed) and the rate limiter run in
    // both cases.
    AudioResult SubmitReplicatedEvent(const AudioEvent& event,
                                       ReplicationSource source)
        AUDIO_REQUIRES(NetworkThread);

    AudioResult UpdateReplicatedTransform(EmitterHandle  handle,
                                           const Vec3&    position,
                                           const Vec3&    forward,
                                           const Vec3&    velocity,
                                           SimulationTick tick)
        AUDIO_REQUIRES(NetworkThread);

    AudioResult OnVoicePacket(AudioPlayerId  playerId,
                               const uint8_t* bytes,
                               size_t         size,
                               uint16_t       sequenceNumber,
                               TimestampMs    timestampMs)
        AUDIO_REQUIRES(NetworkThread);

    // Deterministic overload. The host supplies the arrival time
    // explicitly — typically the host's tick clock — instead of
    // letting the engine sample steady_clock internally. Use this
    // form when you need bit-identical replay of a recorded event
    // sequence: the jitter EMA's adaptive target depth depends on
    // observed inter-arrival timing, which is the only remaining
    // wall-clock-derived source of non-determinism on the voice
    // path. Pass the same timeline values during replay as during
    // recording and the resulting mix is identical.
    //
    // For non-deterministic clients, the 5-arg form above samples
    // steady_clock at the call site and continues working the way
    // it always did.
    AudioResult OnVoicePacket(AudioPlayerId  playerId,
                               const uint8_t* bytes,
                               size_t         size,
                               uint16_t       sequenceNumber,
                               TimestampMs    timestampMs,
                               TimestampMs    arrivalTimestampMs)
        AUDIO_REQUIRES(NetworkThread);

    // ---- Voice (game thread) ---------------------------------------------

    Result<VoiceSourceHandle> RegisterVoiceSource(AudioPlayerId playerId)
        AUDIO_REQUIRES(GameThread);

    AudioResult UnregisterVoiceSource(VoiceSourceHandle handle)
        AUDIO_REQUIRES(GameThread);

    // Per-player voice network telemetry. Surfaces what the jitter
    // buffer sees for that player: packets received, late, lost,
    // duplicate, reordered, plus the current observed inter-arrival
    // jitter and the buffer's adaptive target depth. Use this to
    // drive in-game UI ("voice signal weak", "your network is
    // dropping") without rolling your own packet accounting.
    //
    // Returns false if no voice source is registered for this
    // player. Stat fields are filled via the out parameter.
    struct VoiceNetworkStats {
        uint64_t packetsReceived         = 0;
        uint64_t packetsAccepted         = 0;
        uint64_t packetsLate             = 0;
        uint64_t packetsDuplicate        = 0;
        uint64_t packetsReordered        = 0;
        uint64_t packetsLost             = 0;
        uint64_t packetsOverwritten      = 0;
        // Voice packets the network-thread rate limiter rejected before
        // they reached the jitter buffer. High counts in steady state
        // indicate either an overprovisioned attacker or a misconfigured
        // budget; tune `AudioConfig::replicationRateLimit.perCategory[Voice]`.
        uint64_t packetsRateLimited     = 0;
        uint64_t plcGenerated            = 0;
        uint64_t silentFrames            = 0;
        uint32_t observedJitterMs        = 0;
        uint32_t targetBufferDepthFrames = 0;
    };
    bool GetVoiceNetworkStats(AudioPlayerId playerId, VoiceNetworkStats& out) const
        AUDIO_REQUIRES(GameThread);

    // ---- 2.4 Mute/volume per voice source (game thread) -------------------
    //
    // Per-player mute and volume controls for voice chat. Mute is a
    // full-stop on Opus decode at the control-thread decode boundary:
    // packets still arrive (network thread keeps pushing into the
    // jitter buffer), but DecodeAndPush sees the muted flag and skips
    // codec.Decode + skips PcmRing.Push for that source. CPU savings
    // are real and measurable; the muted player's audio stops within
    // one tick.
    //
    // Volume is partial attenuation applied to decoded int16 PCM
    // before pushing into the ring. Range [0.0, 4.0]; values >1.0
    // boost above unity (clamped to int16 at the multiplication
    // boundary). Default 1.0.
    //
    // Persistence (the "remember across sessions" outcome from spec
    // 2.4) is the HOST's job — gool doesn't own the player database.
    // Hosts query state via Get*; persist; restore via Set* on
    // reconnect.
    AudioResult SetVoiceSourceMuted(AudioPlayerId playerId, bool muted)
        AUDIO_REQUIRES(GameThread);
    AudioResult SetVoiceSourceVolume(AudioPlayerId playerId, float volume)
        AUDIO_REQUIRES(GameThread);
    bool IsVoiceSourceMuted(AudioPlayerId playerId, bool& outMuted) const
        AUDIO_REQUIRES(GameThread);
    bool GetVoiceSourceVolume(AudioPlayerId playerId, float& outVolume) const
        AUDIO_REQUIRES(GameThread);

    // ---- 2.6 Bandwidth budget for outbound voice (game thread) ------------
    //
    // Per-player upstream bandwidth budget for voice. The engine
    // maintains a token bucket and answers SuggestVoiceBitrate(...)
    // with one of {32000, 24000, 16000, 0} bps — host encodes at the
    // suggested rate, or drops the frame if the suggestion is 0.
    //
    // ARCHITECTURE NOTE: gool's engine owns the INBOUND voice path
    // (network → jitter buffer → decode → mix) but DOES NOT own the
    // outbound encode path (mic capture → encode → network). The
    // budget is enforced via consultation: host calls SuggestBitrate
    // before encoding, then calls ReportBytesSent after sending. This
    // keeps the engine's "we don't capture mics or talk to networks"
    // boundary intact while still letting hosts get reliable
    // bandwidth management.
    //
    // Typical host integration loop (50 Hz / 20ms frame):
    //
    //   const int32_t br = runtime.SuggestVoiceBitrate(myId, 20);
    //   if (br == 0) {
    //       // dropped — budget exhausted, don't send anything
    //       continue;
    //   }
    //   encoder.SetBitrate(br);
    //   const uint32_t bytes = encoder.Encode(pcmFrame, packetBuf);
    //   network.Send(packetBuf, bytes);
    //   runtime.ReportVoiceBytesSent(myId, bytes, br);
    //
    // 0 (the default) = unlimited; SuggestBitrate always returns 32000.
    AudioResult SetVoiceBandwidthBudget(AudioPlayerId playerId,
                                          uint32_t bytesPerSec)
        AUDIO_REQUIRES(GameThread);

    // Consult the budget. Returns 32000 / 24000 / 16000 / 0 (drop).
    // Refills the token bucket from elapsed wall time. Safe to call
    // from any thread the host uses for encoding; serialize per-source
    // calls if your encoder is multithreaded.
    int32_t SuggestVoiceBitrate(AudioPlayerId playerId,
                                  uint32_t frameDurationMs);

    // Host reports the actual encoded packet size after sending. The
    // engine deducts bytes from the bucket, bumps Stats::voiceBytesSent,
    // and (if `bitrateUsedBps` reflects a downgraded rung)
    // Stats::voiceFramesBudgetDowngraded.
    AudioResult ReportVoiceBytesSent(AudioPlayerId playerId,
                                       uint32_t      bytes,
                                       int32_t       bitrateUsedBps);

    // ---- Debug / introspection (game thread) ------------------------------
    // Reads a snapshot of stats published by the control thread. Cheap; safe
    // to poll every frame. Render-thread-owned counters are surfaced via
    // atomic loads.
    struct Stats {
        uint32_t activeEmitters            = 0;
        uint32_t activeVoiceSources        = 0;
        uint32_t eventsDrainedLastTick     = 0;
        uint32_t lateEventsDiscardedLastTick = 0;
        uint32_t occlusionChecksLastTick   = 0;
        uint32_t mixerVoicesActive         = 0;
        uint64_t renderUnderruns           = 0;
        uint64_t totalRenderCallbacks      = 0;
        // One-shots dropped because the emitter pool was full of higher-priority
        // sounds (no eviction candidate). Persistent CreateEmitter rejections
        // are reported through the function's return value, not here.
        uint64_t oneShotsDroppedFullPool   = 0;
        // One-shots that triggered eviction of a lower-priority playing one-shot.
        // High = priority system is doing its job under contention.
        uint64_t oneShotEvictions          = 0;
        // Predicted events successfully cancelled (matched an active
        // emitter; faded Stop posted). High after server reconciliation
        // failures means lots of mispredictions; tune your prediction
        // model if this dominates the legitimate event count.
        uint64_t predictionsCancelled            = 0;
        // Predicted-event cancellations that found no matching emitter
        // (already retired, never started, or evicted). Treated as a
        // soft success for the API's caller; surfaced here for
        // diagnostics.
        uint64_t predictionsCancelledNotFound    = 0;
        // Replicated transforms processed this tick. With interest
        // management active, this is bounded by
        // `maxActiveEmittersProcessedPerTick`; without it, equals the
        // number of active spatialized emitters.
        uint32_t emittersProcessedLastTick       = 0;
        // Active emitters that were skipped this tick because the
        // interest-management budget was exhausted (further from the
        // listener than the top-N cutoff). They still exist; their
        // transforms still update; they just didn't get spatial
        // params recomputed this tick.
        uint32_t emittersSkippedByInterestLastTick = 0;
        // Replicated events rejected by the per-player, per-category
        // token-bucket rate limiter, aggregated across all players.
        // Indexed by AudioCategory (0=SFX, 1=Voice, 2=Music,
        // 3=Ambience, 4=UI, 5=Dialogue). High counts in any one
        // category signal a misbehaving (or malicious) client; query
        // GetPerPlayerReplicationStats(playerId) to identify which.
        uint64_t replicationEventsRateLimited[6] = {0,0,0,0,0,0};
        // Replicated events rejected by the IReplicationValidator
        // hook (if installed). Aggregated across all players. This
        // counter reflects ONLY host-policy denials — runtime-level
        // protocol enforcement (Phase 2.5) is tracked separately in
        // `replicationPolicyViolations` below so dashboards can
        // distinguish the two.
        uint64_t replicationEventsRejectedByValidator = 0;
        // Replicated events rejected by Phase 2.5 protocol-policy
        // enforcement: a Client-sourced event declared a
        // ServerAuthoritative policy, which only the server is
        // allowed to author. Non-zero values here indicate active
        // spoof attempts at the wire layer. Each rejection means
        // the spoofed event was dropped before reaching the
        // control thread — no remote listener heard it.
        uint64_t replicationPolicyViolations = 0;
        // Replicated events rejected because the per-tick new-player
        // admission cap was exhausted (anti-DoS for playerId-cycling).
        // Non-zero values in steady state indicate either a
        // legitimately busy lobby (bump
        // `ReplicationRateLimitConfig::maxNewPlayersPerTick`) or an
        // active id-cycling attack.
        uint64_t replicationEventsRejectedNewIdBudget = 0;
        // Exceptions thrown by host-supplied telemetry sink during
        // OnRuntimeStats(). The runtime catches and counts them here
        // so a misbehaving sink can't break Update(); non-zero in
        // steady state means the sink is buggy. Built-in sinks
        // (JsonLines / Prometheus / Ring) never throw.
        uint64_t telemetrySinkExceptions = 0;
        // Same for the log sink's OnLogEvent(). Counted per-event;
        // a sink that throws every call will increment fast.
        uint64_t logSinkExceptions = 0;
        // Approximate resident bytes the engine holds at the current
        // configuration. Computed from AudioConfig + AudioRuntimeBudget
        // via EstimateBaselineBytes() — see memory_budget.h for what
        // this does and doesn't count. Conservative-low; tracking the
        // exact figure would need per-allocation instrumentation.
        // Stable across ticks unless config changes mid-lifetime
        // (which the runtime doesn't currently support).
        uint64_t approxBytesAllocated = 0;

        // ---- 2.4: voice source mute counter -------------------------------
        // Frames the decode boundary dropped because their source was
        // muted via SetVoiceSourceMuted. Summed across all voice sources;
        // monotonic. Use this to verify mute actually saved decode work
        // (CPU profiling will confirm; this counter is the bookkeeping
        // side).
        uint64_t voiceFramesDroppedDueToMute = 0;

        // ---- 2.6: bandwidth budget counters -------------------------------
        // Total bytes the host has reported via ReportVoiceBytesSent
        // (across all voice sources). Monotonic uint64.
        uint64_t voiceBytesSent = 0;

        // Frames where SuggestVoiceBitrate returned a downgraded rung
        // (24000 or 16000 bps) instead of the default 32000. Incremented
        // when the host reports back with `bitrateUsedBps < 32000`.
        // High counts indicate sustained bandwidth pressure.
        uint64_t voiceFramesBudgetDowngraded = 0;

        // Frames where SuggestVoiceBitrate returned 0 (drop). Incremented
        // at the policy decision boundary — the host is expected to NOT
        // call ReportVoiceBytesSent for these. High counts indicate the
        // budget is too tight or the encoder rate is too high; consider
        // raising the budget or accepting lower quality.
        uint64_t voiceFramesBudgetDropped = 0;
    };
    Stats GetStats() const AUDIO_REQUIRES(GameThread);

    // ---- Replication validator + per-player stats (game thread) ---------
    // Install a host-supplied policy hook called from the network thread
    // before per-category rate limiting on every SubmitReplicatedEvent.
    // Returning false from ShouldAccept silently drops the event and
    // increments the validator-rejection counter.
    //
    // Pass nullptr to clear a previously-installed validator. Lifetime
    // is the host's responsibility; the runtime stores the pointer
    // verbatim and never deletes it.
    void SetReplicationValidator(class IReplicationValidator* validator) noexcept
        AUDIO_REQUIRES(GameThread);

    // Per-player replication stats: events accepted by the rate limiter,
    // events dropped by it, events rejected by the validator. Returns
    // false if the player has never been seen (or was evicted from the
    // LRU table when capacity was hit). Cheap; safe to poll every frame.
    struct PerPlayerReplicationStats {
        uint64_t eventsAccepted    = 0;
        uint64_t eventsRateLimited = 0;
        uint64_t eventsRejected    = 0;
    };
    bool GetPerPlayerReplicationStats(AudioPlayerId             playerId,
                                       PerPlayerReplicationStats& out) const
        AUDIO_REQUIRES(GameThread);

private:
    std::unique_ptr<AudioRuntimeImpl> impl_;
};

} // namespace audio

#endif // AUDIO_ENGINE_AUDIO_RUNTIME_H
