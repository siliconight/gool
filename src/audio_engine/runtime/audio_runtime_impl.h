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
#include <unordered_map>
#include <vector>

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
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

    void Update(float deltaSeconds);

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
    AudioResult           SetEffectParameter(BusId    busId,
                                              uint32_t effectIndex,
                                              uint16_t paramId,
                                              float    value);

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

    // Network thread API
    void        OnTickAdvanced(SimulationTick tick, TimestampMs serverTimeMs);
    AudioResult SubmitReplicatedEvent(const AudioEvent& event);
    AudioResult SubmitReplicatedEvent(const AudioEvent& event,
                                       ReplicationSource source);
    AudioResult UpdateReplicatedTransform(EmitterHandle  h,
                                           const Vec3&    pos,
                                           const Vec3&    fwd,
                                           const Vec3&    vel,
                                           SimulationTick tick);
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
