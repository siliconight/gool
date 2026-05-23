// audio_engine/runtime/audio_runtime_impl.h
//
// Pimpl for AudioRuntime. Owns every subsystem and routes work between
// threads. Public methods on AudioRuntime forward into here.
//
// Threading invariants encoded in this file:
//   * Game thread:    Initialize, Shutdown, the registration/CRUD methods,
//                     SubmitEvent, RegisterVoiceSource, GetStats
//   * Network thread: OnTickAdvanced, SubmitReplicatedEvent,
//                     UpdateReplicatedTransform, OnVoicePacket
//   * Control thread: Update (drains all rings, runs spatializer, builds
//                     mixer command stream)
//   * Render thread:  AudioMixer::OnRender, invoked by the backend
//
// Cross-thread data flow uses SPSC rings only. No locks on the audio path.

#ifndef AUDIO_ENGINE_RUNTIME_AUDIO_RUNTIME_IMPL_H
#define AUDIO_ENGINE_RUNTIME_AUDIO_RUNTIME_IMPL_H

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/logging.h"
#include "audio_engine/spatializer.h"
#include "audio_engine/voice_codec.h"
#include "audio_engine/geometry_query.h"
#include "audio_engine/runtime/replication_rate_limiter.h"

#include "audio_engine/assets/audio_asset_registry.h"
#include "audio_engine/emitters/emitter_manager.h"
#include "audio_engine/listeners/listener_manager.h"
#include "audio_engine/voice/voice_source_manager.h"
#include "audio_engine/spatial/occlusion_system.h"
#include "audio_engine/mixer/audio_mixer.h"
#include "audio_engine/mixer/bus_graph.h"
#include "audio_engine/orchestrator/audio_orchestrator.h"
#include "audio_engine/util/spsc_ring.h"

namespace audio {

class IReplicationValidator; // forward decl; full definition in replication_validator.h

// Maximum voice packet size copied into the SPSC ring on the network thread.
// Sized to comfortably hold Opus packets at typical bitrates; oversize is
// fine, the ring is bounded by depth, not bytes-per-element.
constexpr size_t kMaxVoicePacketBytes = 1500;

struct ReplicatedTransformUpdate {
    EmitterHandle  handle{};
    Vec3           position{};
    Vec3           forward{};
    Vec3           velocity{};
    SimulationTick tick = 0;
    // v0.18.0: which subfields in this update are fresh. Phase 4
    // applies only the bits that are set; unmasked components are
    // left at their previous values in the emitter manager's
    // history. Default `All` preserves pre-v0.18.0 behavior where
    // every update wrote every component.
    TransformStateMask mask = TransformStateMask::All;
};

struct VoicePacketCopy {
    AudioPlayerId  playerId       = kInvalidPlayerId;
    uint16_t       sequenceNumber = 0;
    uint16_t       size           = 0;
    TimestampMs    timestampMs    = 0;     // sender-stamped send time
    TimestampMs    arrivalMs      = 0;     // receiver-stamped arrival time (steady_clock-derived)
    std::array<uint8_t, kMaxVoicePacketBytes> bytes{};
};

class AudioRuntimeImpl {
public:
    AudioRuntimeImpl();
    ~AudioRuntimeImpl();

    AudioRuntimeImpl(const AudioRuntimeImpl&)            = delete;
    AudioRuntimeImpl& operator=(const AudioRuntimeImpl&) = delete;

    AudioResult Initialize(const AudioConfig& config, AudioRuntimeDependencies deps);
    void        Shutdown();
    bool        IsInitialized() const noexcept { return initialized_; }

    // v0.22.7: non-owning accessor for the installed backend. Returns
    // backend_.get() — nullptr before Initialize, nullptr after
    // Shutdown (the unique_ptr is moved/reset there). Read-only.
    const IAudioBackend* GetBackend() const noexcept { return backend_.get(); }

    // v0.22.8.1: mixer diagnostic forwarders. AudioMixer's accessor
    // calls are inlined here (the .cpp file gets the full type via
    // audio_mixer.h #include). The binding goes
    // GoolAudioRuntime → AudioRuntime → AudioRuntimeImpl → AudioMixer,
    // never including audio_mixer.h itself — so the binding builds
    // with just include/ on its include path.
    uint32_t GetActiveVoicesApprox() const noexcept;
    float    GetMasterPreGainPeak() const noexcept;
    float    GetMasterGainLinear() const noexcept;
    void     ResetMasterPreGainPeak() noexcept;

    // v0.39.0: emitter pool count for dead-air discrimination.
    // See AudioRuntime::GetActiveEmitterCount() for the rationale.
    uint32_t GetActiveEmitterCount() const noexcept;

    // v0.24.0: per-bus metering for the editor mixer dock. Forwarders
    // to BusGraph (which actually owns the bus list and atomic peaks).
    // Same lifetime semantics as the master-peak accessors above: zero
    // / empty when no bus graph has been built yet.
    uint32_t    GetBusCount() const noexcept;
    const char* GetBusName(uint32_t busIndex) const noexcept;
    uint32_t    GetBusParentIndex(uint32_t busIndex) const noexcept;
    float       ReadAndResetBusPeakLinear(uint32_t busIndex) noexcept;

    // v0.27.0: per-bus mute / solo / effect-bypass state read-out.
    bool        IsBusMuted(uint32_t busIndex) const noexcept;
    bool        IsBusSoloed(uint32_t busIndex) const noexcept;
    bool        IsBusEffectsBypassed(uint32_t busIndex) const noexcept;

    // v0.28.0: effect-chain introspection.
    uint32_t    GetEffectCount(uint32_t busIndex) const noexcept;
    EffectKind  GetEffectKind(uint32_t busIndex,
                               uint32_t effectIndex) const noexcept;
    float       GetEffectParameter(uint32_t busIndex,
                                    uint32_t effectIndex,
                                    uint16_t paramId) const noexcept;

    // v0.25.2: SoundDefinition lookup, forwarded to AudioAssetRegistry.
    const SoundDefinition* GetSoundDefinition(AudioSoundId id) const noexcept;

    // v0.66.0: sound introspection — see audio_runtime.h for design
    // notes. All three forward to the asset registry; the bool/info
    // shapes hide registry types from the public header.
    bool HasPlayableAsset(AudioSoundId id) const noexcept;
    bool GetSoundInfo(AudioSoundId id, SoundAssetInfo& out) const noexcept;
    size_t GetRegisteredSoundCount() const noexcept;

    void Update(float deltaSeconds) noexcept;

    // v0.15.0: the actual Update body, kept separate from the public
    // noexcept entry so the catch-all wrapper in Update() can convert
    // any escaped exception into a telemetry counter + log line
    // without rewriting 400 lines of business logic in a try block.
    // Internal; do not call directly — the noexcept guarantee is on
    // Update().
    void UpdateBody_(float deltaSeconds);

    // ---- v0.16.0: Update phase decomposition --------------------------
    // UpdateBody_ is now a thin orchestrator that calls a sequence of
    // private phase helpers. The phases follow the 12 numbered comment
    // boundaries the original 386-line function already had; each is
    // marked noexcept since the audio control thread should never
    // propagate exceptions (the Update() wrapper provides the
    // catch-all safety net). Cross-phase state flows through the
    // UpdateTickContext value below — phases 2, 3, 5 consume the
    // network-snapshot timestamps that phase 1 produces; the other
    // phases read only class members.
    //
    // Decomposition rationale: function-length and complexity are not
    // gameplay-correctness concerns directly, but they erode CPU
    // i-cache locality on hot paths and they make code reviews harder
    // (a 386-line function fits no one's working memory). NASA's
    // Power-of-Ten safety-critical rules call for ≤60-line functions
    // for exactly these reasons. Phase 9 (per-emitter spatialization)
    // remains the longest at ~155 lines; it stays as a single helper
    // in v0.16.0 and is a candidate for further sub-decomposition.
    struct UpdateTickContext {
        TimestampMs latestSrv;
        TimestampMs prevSrv;
        TimestampMs nowMs;
    };

    UpdateTickContext Update_Phase1_SnapshotNetworkState_(float deltaSeconds) noexcept;
    void Update_Phase2_DrainNetworkEvents_(TimestampMs nowMs) noexcept;
    void Update_Phase3_DrainGameEvents_(TimestampMs nowMs) noexcept;
    void Update_Phase4_ApplyReplicatedTransforms_() noexcept;
    void Update_Phase5_InterpolateTransforms_(
        float deltaSeconds, TimestampMs latestSrv, TimestampMs prevSrv) noexcept;
    void Update_Phase6_TickOrchestrator_(float deltaSeconds) noexcept;
    void Update_Phase7_BuildEmitterSnapshot_() noexcept;
    void Update_Phase8_RunOcclusion_(float deltaSeconds) noexcept;
    void Update_Phase9_SpatializeEmitters_() noexcept;
    void Update_Phase10_DrainVoicePackets_() noexcept;
    void Update_Phase11_TickOneShotsAndPublishStats_(float deltaSeconds) noexcept;

    // v0.19.0 Tier-B: immediate-event drain. Runs at the very top of
    // UpdateBody_ (before Phase 1) so events submitted via
    // SubmitImmediateEvent are processed with sub-tick latency.
    // Bypasses the per-player/per-category rate limiter and the
    // late-event discard policy that Phase 2 applies. The "Phase 0"
    // name is positional, not a renumber of the existing phases.
    void Update_Phase0_DrainImmediateEvents_() noexcept;

    // Game thread API
    AudioResult           RegisterSoundDefinition(const SoundDefinition& def);
    AudioResult           RegisterPcmSound(AudioSoundId id,
                                            std::span<const float> samples,
                                            uint32_t               sampleRate,
                                            uint32_t               channels);
    void                  SetListener(const AudioListener& listener);
    Result<EmitterHandle> CreateEmitter(const EmitterDescriptor& desc);
    AudioResult           DestroyEmitter(EmitterHandle handle, float fadeOutMs = 0.0f);
    AudioResult           SetEmitterTransform(EmitterHandle h,
                                               const Vec3& pos,
                                               const Vec3& fwd,
                                               const Vec3& vel);
    AudioResult           SetEmitterParameter(EmitterHandle    h,
                                               AudioParameterId param,
                                               float            value,
                                               float            smoothingMs);

    // Global (RTPC) parameter store
    AudioResult           SetGlobalParameter(AudioParameterId paramId, float value);
    bool                  GetGlobalParameter(AudioParameterId paramId, float& outValue) const;
    bool                  ClearGlobalParameter(AudioParameterId paramId);
    size_t                GetGlobalParameterCount() const;

    // Sound-level RTPC bindings (multi-target, multi-curve)
    AudioResult           SetSoundRtpc(AudioSoundId             soundId,
                                        const SoundRtpcBinding&  binding);
    bool                  ClearSoundRtpc(AudioSoundId soundId, RtpcTarget target);
    size_t                ClearAllSoundRtpc(AudioSoundId soundId);
    size_t                GetSoundRtpcBindingCount() const;

    AudioResult           SubmitEvent(const AudioEvent& event);
    AudioResult           CancelPredictedEvent(uint64_t predictionId, float fadeOutMs);
    bool                  GetVoiceNetworkStats(AudioPlayerId playerId,
                                                AudioRuntime::VoiceNetworkStats& out) const;

    // Bus / effect parameter control
    AudioResult           SetBusGainDb(BusId busId, float gainDb);
    // v0.27.0: per-bus mute / solo / effect-bypass setters.
    AudioResult           SetBusMuted(BusId busId, bool muted);
    AudioResult           SetBusSoloed(BusId busId, bool soloed);
    AudioResult           SetBusEffectsBypassed(BusId busId, bool bypassed);
    BusId                 FindBusIdByName(std::string_view name) const;
    AudioResult           SetEffectParameter(BusId    busId,
                                              uint32_t effectIndex,
                                              uint16_t paramId,
                                              float    value);

    // v0.31.0 — live occlusion controls. Both flip atomically; the
    // next OcclusionSystem::Update tick sees the new value.
    void                  SetOcclusionEnabled(bool enabled);
    void                  SetOcclusionIntensity(float intensity);

    // Decoded-file registration (game thread).
    AudioResult           RegisterSoundFromFile(AudioSoundId id, const char* path);
    AudioResult           RegisterSoundFromMemory(AudioSoundId             id,
                                                    std::span<const uint8_t> bytes,
                                                    AudioFileFormat          formatHint);
    AudioResult           RegisterStreamingSoundFromFile(AudioSoundId id, const char* path);
    AudioResult           RegisterStreamingSoundFromMemory(AudioSoundId         id,
                                                            std::vector<float>&& samples,
                                                            uint32_t             sampleRate,
                                                            uint32_t             channels);

    Result<VoiceSourceHandle> RegisterVoiceSource(AudioPlayerId playerId);
    AudioResult               UnregisterVoiceSource(VoiceSourceHandle h);

    // 2.4 mute/volume
    AudioResult SetVoiceSourceMuted(AudioPlayerId playerId, bool muted);
    AudioResult SetVoiceSourceVolume(AudioPlayerId playerId, float volume);
    bool        IsVoiceSourceMuted(AudioPlayerId playerId, bool& outMuted) const;
    bool        GetVoiceSourceVolume(AudioPlayerId playerId, float& outVolume) const;

    // 2.6 bandwidth budget
    AudioResult SetVoiceBandwidthBudget(AudioPlayerId playerId,
                                          uint32_t bytesPerSec);
    int32_t     SuggestVoiceBitrate(AudioPlayerId playerId,
                                      uint32_t frameDurationMs);
    AudioResult ReportVoiceBytesSent(AudioPlayerId playerId,
                                       uint32_t bytes,
                                       int32_t  bitrateUsedBps);

    // Network thread API
    void        OnTickAdvanced(SimulationTick tick, TimestampMs serverTimeMs);
    AudioResult SubmitReplicatedEvent(const AudioEvent& event);
    AudioResult SubmitReplicatedEvent(const AudioEvent& event,
                                       ReplicationSource source);
    // v0.18.0 Tier-A: explicit delivery class.
    AudioResult SubmitReplicatedEvent(const AudioEvent& event,
                                       ReplicationSource source,
                                       EventDelivery     delivery);
    // v0.19.0 Tier-B: sub-tick latency path.
    AudioResult SubmitImmediateEvent(const AudioEvent& event,
                                      ReplicationSource source);
    AudioResult UpdateReplicatedTransform(EmitterHandle  h,
                                           const Vec3&    pos,
                                           const Vec3&    fwd,
                                           const Vec3&    vel,
                                           SimulationTick tick);
    // v0.18.0 Tier-A: partial transform update via TransformStateMask.
    AudioResult UpdateReplicatedTransform(EmitterHandle      h,
                                           TransformStateMask mask,
                                           const Vec3&        pos,
                                           const Vec3&        fwd,
                                           const Vec3&        vel,
                                           SimulationTick     tick);
    AudioResult OnVoicePacket(AudioPlayerId  playerId,
                               const uint8_t* bytes,
                               size_t         size,
                               uint16_t       sequenceNumber,
                               TimestampMs    timestampMs);
    AudioResult OnVoicePacket(AudioPlayerId  playerId,
                               const uint8_t* bytes,
                               size_t         size,
                               uint16_t       sequenceNumber,
                               TimestampMs    timestampMs,
                               TimestampMs    arrivalTimestampMs);

    AudioRuntime::Stats GetStats() const;

    // Replication validator + per-player stats (game thread).
    void SetReplicationValidator(IReplicationValidator* validator) noexcept;
    bool GetPerPlayerReplicationStats(
        AudioPlayerId                              playerId,
        AudioRuntime::PerPlayerReplicationStats&   out) const;

private:
    // Step helpers called from Update().
    void     HandleEvent(const AudioEvent& e, bool replicated);
    bool     IsEventTooLate(TimestampMs eventMs, TimestampMs nowMs) const;
    bool     IsEventTooLateWithOverride(TimestampMs eventMs,
                                          TimestampMs nowMs,
                                          uint32_t    overrideMs) const;
    void     StartOneShotForSound(AudioSoundId soundId,
                                    const Vec3& pos,
                                    AudioPriority pri,
                                    uint64_t predictionId = 0);
    void     PostMixerStartForEmitter(EmitterRecord& rec, const PcmAsset& asset);
    void     PostMixerStartStreamingForEmitter(EmitterRecord& rec, StreamingAsset& asset);
    void     PostMixerStopForEmitter(uint32_t mixSlot, float fadeOutMs = 0.0f);
    void     PumpStreamingAssets();

    // Stop the mixer voice for `rec` and reset any streaming asset state it
    // was driving. Does NOT call emitters_->Destroy(); each call site decides
    // whether the slot is being destroyed or just stopped.
    void     StopMixerAndResetStreamingFor(const EmitterRecord& rec, float fadeOutMs = 0.0f);

    // Compute "effective priority" used by the eviction policy. Higher value
    // = harder to evict. Combines the descriptor's static AudioPriority with
    // a distance-to-listener tie-breaker so close sounds beat far sounds at
    // the same priority. Persistent (non-oneShot) emitters are not eligible
    // targets and are reported with INT64_MAX so they never appear as the
    // minimum.
    int64_t  EffectivePriority(const EmitterRecord& rec, const Vec3& listenerPos) const noexcept;
    int64_t  EffectivePriorityForCandidate(AudioPriority pri, const Vec3& pos,
                                            const Vec3& listenerPos) const noexcept;

    // Try to evict the lowest-priority playing one-shot whose effective
    // priority is strictly less than `incoming`. Returns true on eviction
    // (slot freed; caller can retry emitters_->Create). False means no
    // eviction was possible; the incoming sound should be dropped.
    bool     EvictLowestPriorityOneShotIfBeatenBy(int64_t incoming);

    // Resolves the BusId for an emitter: descriptor override if set, else
    // the bus that the emitter's category maps to, else master.
    BusId    ResolveBusForEmitter(const EmitterRecord& rec) const noexcept;

    // Configuration
    AudioConfig config_{};
    bool        initialized_ = false;

    // Subsystems (constructed in Initialize after config is known)
    std::unique_ptr<AudioAssetRegistry> assets_;
    std::unique_ptr<EmitterManager>     emitters_;
    std::unique_ptr<ListenerManager>    listeners_;
    std::unique_ptr<VoiceSourceManager> voices_;
    std::unique_ptr<BusGraph>           busGraph_;
    std::unique_ptr<AudioMixer>         mixer_;
    std::unique_ptr<AudioOrchestrator>  orchestrator_;
    std::unique_ptr<OcclusionSystem>    occlusion_;

    // Injected (or default) seam implementations
    std::unique_ptr<IAudioBackend>       backend_;
    std::unique_ptr<ISpatializer>        spatializer_;
    std::unique_ptr<IAudioGeometryQuery> geometry_;
    std::unique_ptr<IVoiceCodec>         voiceCodec_;

    // Cross-thread rings. All bounded; sized at Initialize.
    std::unique_ptr<util::SpscRing<AudioEvent>>                gameEvents_;
    std::unique_ptr<util::SpscRing<AudioEvent>>                netEvents_;
    std::unique_ptr<util::SpscRing<ReplicatedTransformUpdate>> netTransforms_;
    std::unique_ptr<util::SpscRing<VoicePacketCopy>>           voicePackets_;

    // v0.19.0 Tier-B: immediate-event ring. Small (8 entries), drained
    // at the top of UpdateBody_ before Phase 1. Used by
    // SubmitImmediateEvent for time-critical SFX that must process
    // with sub-tick latency. The capacity is the natural rate limit
    // — Tribes' "8 moves per packet" applied to the audio analog.
    std::unique_ptr<util::SpscRing<AudioEvent>>                immediateEvents_;

    // v0.19.0 Tier-B: atomic shadow array of per-emitter replication
    // priorities. Written by CreateEmitter (control thread) from
    // `EmitterDescriptor::replicationPriority`; read by
    // UpdateReplicatedTransform on the network thread to decide
    // whether to drop the update under ring pressure. Indexed by
    // `EmitterHandle::index` (slot 0 is reserved as the null
    // handle and always reads 0). Sized to maxActiveEmitters + 1
    // at Initialize. Atomic for safe cross-thread reads; relaxed
    // ordering because the value is a hint, not a synchronization
    // signal — using a slightly stale priority is fine.
    std::vector<std::atomic<uint8_t>>                          emitterPriorities_;

    // Network-thread published state
    std::atomic<SimulationTick> latestNetworkTick_{0};
    std::atomic<TimestampMs>    latestServerTimeMs_{0};
    std::atomic<TimestampMs>    previousServerTimeMs_{0};

    // Replication rate limiter + host policy validator. Both are
    // touched only on the network thread for writes; reads from the
    // game thread go through the rate limiter's atomic counters.
    ReplicationRateLimiter      replicationRateLimiter_;
    IReplicationValidator*      replicationValidator_ = nullptr;

    // Control-thread accumulated wall clock (millis) for late-event discard.
    TimestampMs controlClockMs_ = 0;

    // SoA snapshot scratch (resized in Initialize).
    std::vector<SpatialEmitterView> emitterViews_;
    std::vector<Vec3>               slotPositions_;
    std::vector<uint8_t>            slotOccupied_;     // 0/non-zero, not vector<bool>
    std::vector<float>              occlusionAmounts_;
    std::vector<float>              occlusionDamping_;

    // Interest-management scratch: per-tick distance-to-listener sort
    // buffer. Sized once at Initialize to maxActiveEmitters; cleared
    // (not freed) at the start of every tick, refilled with the
    // currently-active emitters, partitioned by std::nth_element to
    // pick the closest N for processing. No allocations per-tick.
    struct InterestSortEntry {
        EmitterHandle  handle;
        float          dist2;
        EmitterRecord* rec;
    };
    std::vector<InterestSortEntry> interestSortScratch_;

    // Control-thread scratch for the streaming pump (chunk we decode each
    // tick before pushing to the ring).
    std::vector<float>              streamingDecodeScratch_;

    // Stats published to game thread.
    AudioRuntime::Stats statsLatest_{};

    // Telemetry sink (host-owned, may be nullptr). When non-null and
    // config_.telemetryIntervalMs > 0, the runtime calls into
    // OnRuntimeStats() at that cadence from EmitTelemetry_(), which
    // is invoked at the end of Update().
    IRuntimeTelemetrySink* telemetrySink_   = nullptr;
    // Accumulated milliseconds since the last telemetry emit. Reset
    // to zero (modulo overshoot) each time we fire.
    TimestampMs            telemetryAccumMs_ = 0;
    void EmitTelemetry_(float deltaSeconds);

    // Event-level log sink (host-owned, may be nullptr). The runtime
    // takes logMutex_ around every OnLogEvent call so sinks don't
    // have to be thread-safe themselves. ShouldLog_() inlines a
    // nullptr+level fast-path so disabled categories cost a branch,
    // not a sink call.
    IRuntimeLogSink*       logSink_         = nullptr;
    mutable std::mutex     logMutex_;

    // Sink-exception counters. Surfaced via Stats. Atomic because
    // log hooks fire from game and network threads; telemetry's is
    // atomic only for symmetry (game-thread only in practice).
    // Loaded with relaxed ordering during GetStats / EmitTelemetry —
    // we don't need a happens-before guarantee, just a non-torn read.
    // Mutable so const Log_ can fetch_add on the catch path.
    mutable std::atomic<uint64_t>  telemetrySinkExceptions_{0};
    mutable std::atomic<uint64_t>  logSinkExceptions_{0};

    // Mixer underrun counter snapshot from the previous Update tick.
    // Used to deduce render-thread underrun events on the game
    // thread without crossing thread boundaries inside the sink call.
    uint64_t               lastUnderruns_   = 0;

    // Fast-path filter check. Returns true if the configured sink
    // should receive an event at `level`. Inlined; cheap when
    // disabled. Members read here (logSink_, config_.logMinLevel)
    // don't change after Initialize, so no lock is needed for the
    // check — only for the actual sink call inside Log_.
    bool ShouldLog_(uint8_t level) const noexcept {
        return logSink_ != nullptr && level >= config_.logMinLevel;
    }
    // LogLevel overload: keeps call sites free of static_cast noise.
    // Equivalent to the uint8 overload (Rule 14: hide implementation
    // details — config_.logMinLevel is uint8 only because logging.h
    // shouldn't have to be pulled into config.h, not because callers
    // should care).
    bool ShouldLog_(LogLevel level) const noexcept {
        return ShouldLog_(static_cast<uint8_t>(level));
    }

    // Emit a log event. Caller must have already checked ShouldLog_;
    // this skips the level filter and goes straight to the sink
    // under logMutex_. The fields span is borrowed; the sink either
    // formats immediately (JSON Lines) or deep-copies (Ring).
    void Log_(uint8_t                            level,
              std::string_view                   category,
              std::string_view                   message,
              std::span<const struct LogField>   fields = {}) const;

    // Tick-to-tick interpolation alpha state (host-clock based).
    float interpAlpha_     = 1.0f;
    float ticksObservedDt_ = 0.05f;     // estimated tick period in seconds

    // Global (RTPC) parameter store. Game-thread-only writes and
    // reads at this stage. Render-thread modulation (sound definitions
    // reading these and adjusting volume / cutoff / pitch) is a
    // future feature; the storage here ships first so host code can
    // build against the API.
    std::unordered_map<AudioParameterId, float> globalParameters_;

    // Sound-level RTPC bindings: a lookup table from soundId to the
    // list of bindings registered for it. At most one binding per
    // (soundId, target) pair; SetSoundRtpc replaces an existing target
    // rather than appending. Walked once per Update tick by
    // EvaluateRtpcBindings_(); each binding evaluates in constant time.
    // Game-thread-only access.
    //
    // Vector capacity caps at kRtpcTargetCount per sound (four targets).
    // The map size as a whole is capped by AudioConfig::maxSoundRtpcBindings
    // — counted as the total number of bindings across all sounds, not
    // distinct sound IDs, so a sound with 4 bindings counts as 4.
    std::unordered_map<AudioSoundId, std::vector<SoundRtpcBinding>> soundRtpcBindings_;

    // Sum of vector sizes across soundRtpcBindings_. Maintained on Set
    // and Clear so GetSoundRtpcBindingCount is O(1).
    size_t soundRtpcBindingTotal_ = 0;

    // Per-tick: walk active emitters, look up each emitter's soundId
    // in soundRtpcBindings_, evaluate every binding in the list against
    // the global parameter store (skipping bindings whose parameter is
    // unset), push each resulting target value into the smoother for
    // the appropriate AudioParameterId.
    void EvaluateRtpcBindings_();
};

} // namespace audio

#endif // AUDIO_ENGINE_RUNTIME_AUDIO_RUNTIME_IMPL_H
