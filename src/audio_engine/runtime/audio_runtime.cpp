// audio_engine/runtime/audio_runtime.cpp
//
// Implementation of AudioRuntime (forwarding to Impl) and AudioRuntimeImpl.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/runtime/audio_runtime_impl.h"
#include "audio_engine/memory_budget.h"
#include "audio_engine/replication_validator.h"
#include "audio_engine/telemetry.h"
#include "audio_engine/logging.h"

#include "audio_engine/backend/null_audio_backend.h"
#include "audio_engine/spatial/default_spatializer.h"
#include "audio_engine/spatial/null_geometry_query.h"
#include "audio_engine/voice/stub_voice_codec.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>  // std::declval — used by v0.15.0 noexcept contract pins

namespace audio {

// ===== AudioRuntime (public) ===============================================

AudioRuntime::AudioRuntime() : impl_(std::make_unique<AudioRuntimeImpl>()) {}
AudioRuntime::~AudioRuntime() = default;

AudioResult AudioRuntime::Initialize(const AudioConfig& config,
                                       AudioRuntimeDependencies deps) {
    return impl_->Initialize(config, std::move(deps));
}
void AudioRuntime::Shutdown()                  { impl_->Shutdown(); }
bool AudioRuntime::IsInitialized() const noexcept { return impl_->IsInitialized(); }
void AudioRuntime::Update(float dt) noexcept     { impl_->Update(dt); }

AudioResult AudioRuntime::RegisterSoundDefinition(const SoundDefinition& d) {
    return impl_->RegisterSoundDefinition(d);
}
AudioResult AudioRuntime::RegisterPcmSound(AudioSoundId id,
                                             std::span<const float> samples,
                                             uint32_t sr,
                                             uint32_t ch) {
    return impl_->RegisterPcmSound(id, samples, sr, ch);
}
AudioResult AudioRuntime::RegisterSoundFromFile(AudioSoundId id, const char* path) {
    return impl_->RegisterSoundFromFile(id, path);
}
AudioResult AudioRuntime::RegisterSoundFromMemory(AudioSoundId             id,
                                                    std::span<const uint8_t> bytes,
                                                    AudioFileFormat          formatHint) {
    return impl_->RegisterSoundFromMemory(id, bytes, formatHint);
}
AudioResult AudioRuntime::RegisterStreamingSoundFromFile(AudioSoundId id, const char* path) {
    return impl_->RegisterStreamingSoundFromFile(id, path);
}
AudioResult AudioRuntime::RegisterStreamingSoundFromMemory(AudioSoundId         id,
                                                            std::vector<float>&& samples,
                                                            uint32_t             sampleRate,
                                                            uint32_t             channels) {
    return impl_->RegisterStreamingSoundFromMemory(id, std::move(samples), sampleRate, channels);
}
void AudioRuntime::SetListener(const AudioListener& l) { impl_->SetListener(l); }

Result<EmitterHandle> AudioRuntime::CreateEmitter(const EmitterDescriptor& d) {
    return impl_->CreateEmitter(d);
}
AudioResult AudioRuntime::DestroyEmitter(EmitterHandle h, float fadeOutMs) { return impl_->DestroyEmitter(h, fadeOutMs); }
AudioResult AudioRuntime::SetEmitterTransform(EmitterHandle h,
                                                const Vec3& p,
                                                const Vec3& f,
                                                const Vec3& v) {
    return impl_->SetEmitterTransform(h, p, f, v);
}
AudioResult AudioRuntime::SetEmitterParameter(EmitterHandle h,
                                                AudioParameterId p,
                                                float val,
                                                float sm) {
    return impl_->SetEmitterParameter(h, p, val, sm);
}
AudioResult AudioRuntime::SetGlobalParameter(AudioParameterId p, float v) {
    return impl_->SetGlobalParameter(p, v);
}
bool AudioRuntime::GetGlobalParameter(AudioParameterId p, float& out) const {
    return impl_->GetGlobalParameter(p, out);
}
bool AudioRuntime::ClearGlobalParameter(AudioParameterId p) {
    return impl_->ClearGlobalParameter(p);
}
size_t AudioRuntime::GetGlobalParameterCount() const {
    return impl_->GetGlobalParameterCount();
}
AudioResult AudioRuntime::SetSoundRtpc(AudioSoundId            sid,
                                         const SoundRtpcBinding& b) {
    return impl_->SetSoundRtpc(sid, b);
}
bool AudioRuntime::ClearSoundRtpc(AudioSoundId sid, RtpcTarget tgt) {
    return impl_->ClearSoundRtpc(sid, tgt);
}
size_t AudioRuntime::ClearAllSoundRtpc(AudioSoundId sid) {
    return impl_->ClearAllSoundRtpc(sid);
}
size_t AudioRuntime::GetSoundRtpcBindingCount() const {
    return impl_->GetSoundRtpcBindingCount();
}
AudioResult AudioRuntime::SubmitEvent(const AudioEvent& e) { return impl_->SubmitEvent(e); }
AudioResult AudioRuntime::CancelPredictedEvent(uint64_t predictionId, float fadeOutMs) {
    return impl_->CancelPredictedEvent(predictionId, fadeOutMs);
}
bool AudioRuntime::GetVoiceNetworkStats(AudioPlayerId playerId,
                                          VoiceNetworkStats& out) const {
    return impl_->GetVoiceNetworkStats(playerId, out);
}

AudioResult AudioRuntime::SetBusGainDb(BusId busId, float gainDb) {
    return impl_->SetBusGainDb(busId, gainDb);
}
BusId AudioRuntime::FindBusIdByName(std::string_view name) const {
    return impl_->FindBusIdByName(name);
}
AudioResult AudioRuntime::SetEffectParameter(BusId    busId,
                                              uint32_t effectIndex,
                                              uint16_t paramId,
                                              float    value) {
    return impl_->SetEffectParameter(busId, effectIndex, paramId, value);
}

void AudioRuntime::OnTickAdvanced(SimulationTick t, TimestampMs ms) {
    impl_->OnTickAdvanced(t, ms);
}
AudioResult AudioRuntime::SubmitReplicatedEvent(const AudioEvent& e) {
    return impl_->SubmitReplicatedEvent(e);
}
AudioResult AudioRuntime::SubmitReplicatedEvent(const AudioEvent& e,
                                                  ReplicationSource s) {
    return impl_->SubmitReplicatedEvent(e, s);
}
AudioResult AudioRuntime::SubmitReplicatedEvent(const AudioEvent& e,
                                                  ReplicationSource s,
                                                  EventDelivery     d) {
    return impl_->SubmitReplicatedEvent(e, s, d);
}
AudioResult AudioRuntime::UpdateReplicatedTransform(EmitterHandle h,
                                                      const Vec3& p,
                                                      const Vec3& f,
                                                      const Vec3& v,
                                                      SimulationTick t) {
    return impl_->UpdateReplicatedTransform(h, p, f, v, t);
}
AudioResult AudioRuntime::UpdateReplicatedTransform(EmitterHandle      h,
                                                      TransformStateMask m,
                                                      const Vec3& p,
                                                      const Vec3& f,
                                                      const Vec3& v,
                                                      SimulationTick t) {
    return impl_->UpdateReplicatedTransform(h, m, p, f, v, t);
}
AudioResult AudioRuntime::OnVoicePacket(AudioPlayerId pid,
                                          const uint8_t* b,
                                          size_t s,
                                          uint16_t seq,
                                          TimestampMs ts) {
    return impl_->OnVoicePacket(pid, b, s, seq, ts);
}

AudioResult AudioRuntime::OnVoicePacket(AudioPlayerId pid,
                                          const uint8_t* b,
                                          size_t s,
                                          uint16_t seq,
                                          TimestampMs ts,
                                          TimestampMs arrivalMs) {
    return impl_->OnVoicePacket(pid, b, s, seq, ts, arrivalMs);
}

Result<VoiceSourceHandle> AudioRuntime::RegisterVoiceSource(AudioPlayerId p) {
    return impl_->RegisterVoiceSource(p);
}
AudioResult AudioRuntime::UnregisterVoiceSource(VoiceSourceHandle h) {
    return impl_->UnregisterVoiceSource(h);
}

// --- 2.4 mute/volume proxies. impl_ owns the VoiceSourceManager; we
// forward to its setters directly. Returns NotInitialized if the
// runtime hasn't been Initialize()'d.
AudioResult AudioRuntime::SetVoiceSourceMuted(AudioPlayerId p, bool muted) {
    return impl_->SetVoiceSourceMuted(p, muted);
}
AudioResult AudioRuntime::SetVoiceSourceVolume(AudioPlayerId p, float volume) {
    return impl_->SetVoiceSourceVolume(p, volume);
}
bool AudioRuntime::IsVoiceSourceMuted(AudioPlayerId p, bool& out) const {
    return impl_->IsVoiceSourceMuted(p, out);
}
bool AudioRuntime::GetVoiceSourceVolume(AudioPlayerId p, float& out) const {
    return impl_->GetVoiceSourceVolume(p, out);
}

// --- 2.6 bandwidth budget proxies.
AudioResult AudioRuntime::SetVoiceBandwidthBudget(AudioPlayerId p,
                                                    uint32_t bytesPerSec) {
    return impl_->SetVoiceBandwidthBudget(p, bytesPerSec);
}
int32_t AudioRuntime::SuggestVoiceBitrate(AudioPlayerId p, uint32_t frameMs) {
    return impl_->SuggestVoiceBitrate(p, frameMs);
}
AudioResult AudioRuntime::ReportVoiceBytesSent(AudioPlayerId p,
                                                 uint32_t bytes,
                                                 int32_t bitrateUsedBps) {
    return impl_->ReportVoiceBytesSent(p, bytes, bitrateUsedBps);
}

AudioRuntime::Stats AudioRuntime::GetStats() const { return impl_->GetStats(); }

void AudioRuntime::SetReplicationValidator(IReplicationValidator* v) noexcept {
    impl_->SetReplicationValidator(v);
}
bool AudioRuntime::GetPerPlayerReplicationStats(
        AudioPlayerId                              playerId,
        AudioRuntime::PerPlayerReplicationStats&   out) const {
    return impl_->GetPerPlayerReplicationStats(playerId, out);
}

// ===== AudioRuntimeImpl ====================================================

AudioRuntimeImpl::AudioRuntimeImpl()  = default;
AudioRuntimeImpl::~AudioRuntimeImpl() {
    if (initialized_) Shutdown();
}

AudioResult AudioRuntimeImpl::Initialize(const AudioConfig& config,
                                          AudioRuntimeDependencies deps) {
    if (initialized_) return AudioResult::AlreadyInitialized;

    config_ = config;

    // Inject defaults for null seams.
    spatializer_ = deps.spatializer
        ? std::move(deps.spatializer)
        : std::unique_ptr<ISpatializer>(new DefaultSpatializer());
    geometry_    = deps.geometryQuery
        ? std::move(deps.geometryQuery)
        : std::unique_ptr<IAudioGeometryQuery>(new NullGeometryQuery());
    voiceCodec_  = deps.voiceCodec
        ? std::move(deps.voiceCodec)
        : std::unique_ptr<IVoiceCodec>(new StubVoiceCodec(config.sampleRate, 1u, 960u));
    backend_     = deps.backend
        ? std::move(deps.backend)
        : std::unique_ptr<IAudioBackend>(new NullAudioBackend());

    // Telemetry: raw pointer ownership stays with the host. Captured
    // here for fast access from Update; nullptr means telemetry is
    // disabled even if telemetryIntervalMs > 0.
    telemetrySink_   = deps.telemetrySink;
    telemetryAccumMs_ = 0;

    // Event-level log sink: same ownership pattern. ShouldLog_ fast-
    // paths via the nullptr check so disabled logging costs a branch
    // per call site, not a sink invocation.
    logSink_         = deps.logSink;
    lastUnderruns_   = 0;

    // Reserve hash-table capacity up front. The configured caps
    // (maxGlobalParameters, maxSoundRtpcBindings) bound steady-state
    // size, so reserving once eliminates the rehash bursts during
    // the first dozen inserts. Soft-divide bindings by typical
    // bindings-per-sound (~4 targets) — over-reservation here costs
    // a few KB of buckets, which is fine; under-reservation just
    // means a couple extra rehashes.
    globalParameters_.reserve(config.maxGlobalParameters);
    soundRtpcBindings_.reserve(
        std::max<uint32_t>(1, config.maxSoundRtpcBindings / 4));

    // Subsystems
    assets_       = std::make_unique<AudioAssetRegistry>(config.budget.maxRegisteredSounds);
    emitters_     = std::make_unique<EmitterManager>(config.budget.maxActiveEmitters);
    listeners_    = std::make_unique<ListenerManager>();

    const uint32_t mixSlotsForEmitters = config.budget.maxActiveEmitters + 1;     // +1 for index 0
    const uint32_t voiceMixSlotBase    = mixSlotsForEmitters;
    const uint32_t totalMixVoices      = mixSlotsForEmitters + config.budget.maxVoiceSources;

    voices_       = std::make_unique<VoiceSourceManager>(
        config.budget.maxVoiceSources,
        config.voicePcmRingFrames,
        config.voicePacketRingDepth,
        config.voiceMaxPacketBytes,
        voiceMixSlotBase);

    const uint32_t outputChannels = (config.outputMode == AudioOutputMode::Stereo) ? 2u : 1u;

    // Build the bus graph from config (or auto-create master-only graph if
    // the host left it empty). Must precede the mixer so we can hand it the
    // graph pointer at construction.
    busGraph_ = std::make_unique<BusGraph>();
    if (auto rc = busGraph_->Build(config.busGraph,
                                     config.sampleRate,
                                     outputChannels,
                                     config.bufferSize); rc != AudioResult::Success) {
        Shutdown();
        return rc;
    }

    mixer_ = std::make_unique<AudioMixer>(totalMixVoices,
                                            outputChannels,
                                            /*commandRingDepth=*/totalMixVoices * 4u + 64u,
                                            busGraph_.get(),
                                            config.sampleRate);

    orchestrator_ = std::make_unique<AudioOrchestrator>();
    occlusion_    = std::make_unique<OcclusionSystem>(config.budget.maxActiveEmitters,
                                                        config.budget.maxOcclusionChecksPerFrame);
    occlusion_->SetGeometryQuery(geometry_.get());

    // SPSC rings
    gameEvents_    = std::make_unique<util::SpscRing<AudioEvent>>(
        config.budget.maxGameEventsPerFrame * 2);
    netEvents_     = std::make_unique<util::SpscRing<AudioEvent>>(
        config.budget.maxNetworkEventsPerFrame * 2);
    netTransforms_ = std::make_unique<util::SpscRing<ReplicatedTransformUpdate>>(
        config.budget.maxActiveEmitters * 2);
    voicePackets_  = std::make_unique<util::SpscRing<VoicePacketCopy>>(
        config.voicePacketRingDepth * config.budget.maxVoiceSources);

    // SoA snapshot scratch
    emitterViews_.assign(static_cast<size_t>(config.budget.maxActiveEmitters) + 1,
                          SpatialEmitterView{});
    slotPositions_.assign(static_cast<size_t>(config.budget.maxActiveEmitters) + 1,
                           Vec3{});
    slotOccupied_.assign(static_cast<size_t>(config.budget.maxActiveEmitters) + 1,
                          0u);
    occlusionAmounts_.assign(static_cast<size_t>(config.budget.maxActiveEmitters) + 1,
                              0.0f);
    occlusionDamping_.assign(static_cast<size_t>(config.budget.maxActiveEmitters) + 1,
                              0.0f);

    // Interest-management scratch: reserve to the maximum the runtime
    // could ever fill in one tick, then clear() on each tick to keep
    // size==0 with capacity preserved (no per-tick allocation).
    interestSortScratch_.clear();
    interestSortScratch_.reserve(static_cast<size_t>(config.budget.maxActiveEmitters) + 1);

    // Streaming pump scratch; sized for a worst-case decode chunk at stereo.
    streamingDecodeScratch_.assign(
        static_cast<size_t>(config.streamingDecodeChunkFrames) * 2u, 0.0f);

    // Replication rate limiter: per-player, per-category token buckets.
    // Sized once from config; allocates nothing thereafter.
    replicationRateLimiter_.Initialize(config.replicationRateLimit);
    replicationValidator_ = nullptr;

    // Start backend last; once Start returns, OnRender can fire on the
    // render thread. Mixer is fully constructed by now.
    AudioBackendConfig bc;
    bc.sampleRate = config.sampleRate;
    bc.bufferSize = config.bufferSize;
    bc.channels   = outputChannels;
    if (auto rc = backend_->Start(bc, mixer_.get()); rc != AudioResult::Success) {
        Shutdown();
        return rc;
    }

    initialized_ = true;
    return AudioResult::Success;
}

void AudioRuntimeImpl::Shutdown() {
    if (backend_) backend_->Stop();
    backend_.reset();

    occlusion_.reset();
    orchestrator_.reset();
    mixer_.reset();
    voices_.reset();
    listeners_.reset();
    emitters_.reset();
    assets_.reset();

    voicePackets_.reset();
    netTransforms_.reset();
    netEvents_.reset();
    gameEvents_.reset();

    spatializer_.reset();
    geometry_.reset();
    voiceCodec_.reset();

    initialized_ = false;
}

// ---- Game thread API ------------------------------------------------------

AudioResult AudioRuntimeImpl::RegisterSoundDefinition(const SoundDefinition& def) {
    if (!initialized_) return AudioResult::NotInitialized;
    return assets_->RegisterDefinition(def);
}

AudioResult AudioRuntimeImpl::RegisterPcmSound(AudioSoundId id,
                                                 std::span<const float> samples,
                                                 uint32_t sampleRate,
                                                 uint32_t channels) {
    if (!initialized_) return AudioResult::NotInitialized;
    return assets_->RegisterPcm(id, samples, sampleRate, channels);
}

AudioResult AudioRuntimeImpl::RegisterSoundFromFile(AudioSoundId id, const char* path) {
    if (!initialized_) return AudioResult::NotInitialized;
    const uint32_t outputChannels =
        (config_.outputMode == AudioOutputMode::Mono) ? 1u : 2u;
    return assets_->RegisterDecodedFromFile(id, path,
                                              config_.sampleRate, outputChannels);
}

AudioResult AudioRuntimeImpl::RegisterSoundFromMemory(AudioSoundId             id,
                                                       std::span<const uint8_t> bytes,
                                                       AudioFileFormat          formatHint) {
    if (!initialized_) return AudioResult::NotInitialized;
    const uint32_t outputChannels =
        (config_.outputMode == AudioOutputMode::Mono) ? 1u : 2u;
    return assets_->RegisterDecodedFromMemory(id, bytes, formatHint,
                                                config_.sampleRate, outputChannels);
}

AudioResult AudioRuntimeImpl::RegisterStreamingSoundFromFile(AudioSoundId id, const char* path) {
    if (!initialized_) return AudioResult::NotInitialized;
    const uint32_t outputChannels =
        (config_.outputMode == AudioOutputMode::Mono) ? 1u : 2u;
    return assets_->RegisterStreamingFromFile(id, path,
                                                config_.sampleRate, outputChannels,
                                                config_.streamingRingFrames);
}

AudioResult AudioRuntimeImpl::RegisterStreamingSoundFromMemory(AudioSoundId         id,
                                                                std::vector<float>&& samples,
                                                                uint32_t             sampleRate,
                                                                uint32_t             channels) {
    if (!initialized_) return AudioResult::NotInitialized;
    const uint32_t outputChannels =
        (config_.outputMode == AudioOutputMode::Mono) ? 1u : 2u;
    return assets_->RegisterStreamingFromMemory(id, std::move(samples),
                                                  sampleRate, channels,
                                                  config_.sampleRate, outputChannels,
                                                  config_.streamingRingFrames);
}

void AudioRuntimeImpl::SetListener(const AudioListener& listener) {
    if (!initialized_) return;
    listeners_->SetPrimary(listener);
}

Result<EmitterHandle> AudioRuntimeImpl::CreateEmitter(const EmitterDescriptor& desc) {
    if (!initialized_) return AudioResult::NotInitialized;
    auto h = emitters_->Create(desc, /*oneShot=*/false);
    if (!h) return h.error();

    // For looping emitters with a sound, kick off mixer immediately if asset
    // is preloaded. One-shots are routed through the event path.
    if (auto* rec = emitters_->Get(h.value())) {
        if (rec->descriptor.isLooping && rec->descriptor.soundId != kInvalidSoundId) {
            // Streaming asset takes priority; registered streaming sounds
            // never cohabitate with a pinned PcmAsset under the same id.
            if (auto* stream = assets_->GetStreamingAsset(rec->descriptor.soundId)) {
                if (stream->state != StreamingAsset::State::Idle) {
                    // The one-instance-per-streaming-asset constraint kicks in.
                    // Tear down the just-created emitter and surface the
                    // failure so the host knows the request was rejected.
                    emitters_->Destroy(h.value());
                    return AudioResult::BudgetExceeded;
                }
                stream->looping = rec->descriptor.isLooping;
                PostMixerStartStreamingForEmitter(*rec, *stream);
            } else if (auto* asset = assets_->GetAsset(rec->descriptor.soundId)) {
                PostMixerStartForEmitter(*rec, *asset);
            }
        }
    }
    return h.value();
}

AudioResult AudioRuntimeImpl::DestroyEmitter(EmitterHandle handle, float fadeOutMs) {
    if (!initialized_) return AudioResult::NotInitialized;
    if (auto* rec = emitters_->Get(handle); rec && rec->mixerStarted) {
        StopMixerAndResetStreamingFor(*rec, std::max(0.0f, fadeOutMs));
    }
    orchestrator_->Smoother().Forget(handle);
    return emitters_->Destroy(handle);
}

AudioResult AudioRuntimeImpl::SetEmitterTransform(EmitterHandle h,
                                                    const Vec3& pos,
                                                    const Vec3& fwd,
                                                    const Vec3& vel) {
    if (!initialized_) return AudioResult::NotInitialized;
    return emitters_->SetTransform(h, pos, fwd, vel);
}

AudioResult AudioRuntimeImpl::SetEmitterParameter(EmitterHandle    h,
                                                    AudioParameterId param,
                                                    float            value,
                                                    float            smoothingMs) {
    if (!initialized_) return AudioResult::NotInitialized;
    if (!emitters_->IsValid(h)) return AudioResult::InvalidHandle;
    orchestrator_->Smoother().SetTarget(h, param, value, smoothingMs);
    return AudioResult::Success;
}

AudioResult AudioRuntimeImpl::SetGlobalParameter(AudioParameterId paramId,
                                                   float            value) {
    if (!initialized_)              return AudioResult::NotInitialized;
    if (paramId == kInvalidParameterId) return AudioResult::InvalidArgument;
    auto it = globalParameters_.find(paramId);
    if (it != globalParameters_.end()) {
        // Updating an existing parameter — never exceeds the budget.
        it->second = value;
        return AudioResult::Success;
    }
    // New parameter: enforce the configured cap.
    if (globalParameters_.size() >= config_.maxGlobalParameters) {
        return AudioResult::BudgetExceeded;
    }
    globalParameters_.emplace(paramId, value);
    return AudioResult::Success;
}

bool AudioRuntimeImpl::GetGlobalParameter(AudioParameterId paramId,
                                            float&           outValue) const {
    auto it = globalParameters_.find(paramId);
    if (it == globalParameters_.end()) return false;
    outValue = it->second;
    return true;
}

bool AudioRuntimeImpl::ClearGlobalParameter(AudioParameterId paramId) {
    return globalParameters_.erase(paramId) > 0;
}

size_t AudioRuntimeImpl::GetGlobalParameterCount() const {
    return globalParameters_.size();
}

AudioResult AudioRuntimeImpl::SetSoundRtpc(AudioSoundId            soundId,
                                             const SoundRtpcBinding& binding) {
    if (!initialized_)                       return AudioResult::NotInitialized;
    if (soundId == kInvalidSoundId)          return AudioResult::InvalidArgument;
    if (binding.paramId == kInvalidParameterId)
                                              return AudioResult::InvalidArgument;
    if (binding.minValue == binding.maxValue) return AudioResult::InvalidArgument;
    // Reject NaN/Inf in any of the float fields. The per-tick evaluator
    // assumes finite math; a single bad binding would poison voices on
    // every tick.
    if (!std::isfinite(binding.minValue)     || !std::isfinite(binding.maxValue)     ||
        !std::isfinite(binding.minOutput)    || !std::isfinite(binding.maxOutput)    ||
        !std::isfinite(binding.smoothingMs)  || binding.smoothingMs < 0.0f           ||
        !std::isfinite(binding.curveExponent) || binding.curveExponent < 0.0f) {
        return AudioResult::InvalidArgument;
    }
    // Validate enum values fall in their declared range. uint8 encoding
    // means a host could pass garbage from a bound script context.
    if (static_cast<size_t>(binding.target) >= kRtpcTargetCount) {
        return AudioResult::InvalidArgument;
    }
    if (static_cast<uint8_t>(binding.curve) > static_cast<uint8_t>(RtpcCurve::SCurve)) {
        return AudioResult::InvalidArgument;
    }

    auto& vec = soundRtpcBindings_[soundId];
    // Replace existing binding for this target if present (no error,
    // just update).
    for (auto& existing : vec) {
        if (existing.target == binding.target) {
            existing = binding;
            return AudioResult::Success;
        }
    }
    // New binding for this (soundId, target) pair. Enforce the global
    // budget on total bindings across all sounds.
    if (soundRtpcBindingTotal_ >= config_.maxSoundRtpcBindings) {
        // If the vector was just default-constructed by operator[] above
        // and is empty, erase it so we don't leak an empty entry.
        if (vec.empty()) soundRtpcBindings_.erase(soundId);
        if (ShouldLog_(LogLevel::Warn)) {
            const LogField fields[] = {
                LogField::UInt("sound_id",   static_cast<uint64_t>(soundId)),
                LogField::UInt("param_id",   static_cast<uint64_t>(binding.paramId)),
                LogField::UInt("target",     static_cast<uint64_t>(binding.target)),
                LogField::UInt("budget",     static_cast<uint64_t>(config_.maxSoundRtpcBindings)),
                LogField::UInt("current",    static_cast<uint64_t>(soundRtpcBindingTotal_)),
            };
            Log_(static_cast<uint8_t>(LogLevel::Warn),
                  LogCategory::kRtpc,
                  "RTPC binding rejected: budget exceeded",
                  fields);
        }
        return AudioResult::BudgetExceeded;
    }
    vec.push_back(binding);
    ++soundRtpcBindingTotal_;
    return AudioResult::Success;
}

bool AudioRuntimeImpl::ClearSoundRtpc(AudioSoundId soundId, RtpcTarget target) {
    auto it = soundRtpcBindings_.find(soundId);
    if (it == soundRtpcBindings_.end()) return false;
    auto& vec = it->second;
    for (auto vIt = vec.begin(); vIt != vec.end(); ++vIt) {
        if (vIt->target == target) {
            vec.erase(vIt);
            --soundRtpcBindingTotal_;
            // Clean up the map entry if no bindings remain.
            if (vec.empty()) soundRtpcBindings_.erase(it);
            return true;
        }
    }
    return false;
}

size_t AudioRuntimeImpl::ClearAllSoundRtpc(AudioSoundId soundId) {
    auto it = soundRtpcBindings_.find(soundId);
    if (it == soundRtpcBindings_.end()) return 0;
    const size_t removed = it->second.size();
    soundRtpcBindings_.erase(it);
    soundRtpcBindingTotal_ -= removed;
    return removed;
}

size_t AudioRuntimeImpl::GetSoundRtpcBindingCount() const {
    return soundRtpcBindingTotal_;
}

namespace {

// Map an RtpcTarget to the well-known AudioParameterId on the smoother.
// Kept in sync with the combiner choices in Update step 9.
constexpr AudioParameterId TargetToSmootherParam(RtpcTarget t) {
    switch (t) {
        case RtpcTarget::Volume:        return AudioParameterIds::Gain;
        case RtpcTarget::Pitch:         return AudioParameterIds::Pitch;
        case RtpcTarget::LowPassCutoff: return AudioParameterIds::LowPassAmount;
        case RtpcTarget::ReverbSend:    return AudioParameterIds::ReverbSend;
    }
    return AudioParameterIds::Gain;
}

// Apply a curve to t in [0, 1], producing a value in [0, 1].
inline float ApplyCurve(float t, RtpcCurve curve, float exponent) {
    switch (curve) {
        case RtpcCurve::Linear:
            return t;
        case RtpcCurve::Exponential:
            // pow(0, 0) is 1 in C++; clamp exponent to avoid that
            // surprise for misconfigured bindings (still funny but
            // less footgun-y).
            return std::pow(t, std::max(exponent, 1e-6f));
        case RtpcCurve::InverseExponential:
            return 1.0f - std::pow(1.0f - t, std::max(exponent, 1e-6f));
        case RtpcCurve::SCurve:
            return t * t * (3.0f - 2.0f * t);
    }
    return t;
}

} // namespace

void AudioRuntimeImpl::EvaluateRtpcBindings_() {
    // Cheap fast-path: nothing to do if no bindings are registered.
    // Avoids the ForEach overhead on the typical "no RTPC" project.
    if (soundRtpcBindings_.empty()) return;

    emitters_->ForEach([this](EmitterHandle h, EmitterRecord& rec) {
        const AudioSoundId sid = rec.activeSoundId;
        if (sid == kInvalidSoundId) return;

        auto bIt = soundRtpcBindings_.find(sid);
        if (bIt == soundRtpcBindings_.end()) return;

        for (const auto& binding : bIt->second) {
            // Skip-when-unset: if this binding's parameter has never
            // been set via SetGlobalParameter, leave the authored
            // value in place. Other bindings on the same sound still
            // evaluate.
            auto pIt = globalParameters_.find(binding.paramId);
            if (pIt == globalParameters_.end()) continue;
            const float paramValue = pIt->second;

            // Input remap. minValue == maxValue was rejected at bind
            // time so the divisor is non-zero.
            float t = (paramValue - binding.minValue) /
                      (binding.maxValue - binding.minValue);
            if (t < 0.0f) t = 0.0f;
            else if (t > 1.0f) t = 1.0f;

            // Curve.
            const float shaped = ApplyCurve(t, binding.curve, binding.curveExponent);

            // Output remap. minOutput > maxOutput is allowed and gives
            // an inverted binding (heartbeat-louder-as-health-drops).
            const float output = binding.minOutput +
                                 shaped * (binding.maxOutput - binding.minOutput);

            // Push into the smoother on the matching parameter ID.
            // The orchestrator's per-emitter UpdateParams pass picks
            // these up next tick. Note: the combiner used in step 9
            // depends on the target — see TargetToSmootherParam and
            // the corresponding sp.gain/pitch/lowPass/reverbSend paths.
            orchestrator_->Smoother().SetTarget(
                h, TargetToSmootherParam(binding.target),
                output, binding.smoothingMs);
        }
    });
}

AudioResult AudioRuntimeImpl::SubmitEvent(const AudioEvent& event) {
    if (!initialized_) return AudioResult::NotInitialized;
    return gameEvents_->Push(event) ? AudioResult::Success : AudioResult::QueueFull;
}

AudioResult AudioRuntimeImpl::CancelPredictedEvent(uint64_t predictionId, float fadeOutMs) {
    if (!initialized_)        return AudioResult::NotInitialized;
    if (predictionId == 0)    return AudioResult::InvalidArgument;

    // Linear scan over active emitters. The emitter pool is bounded
    // (typically 16-128) so this is cheap; keeps us off a separate
    // map structure whose lifetime we'd have to reconcile with both
    // natural retire and eviction. predictionId is cleared the moment
    // the slot is freed, so a stale id never matches a recycled
    // emitter by accident.
    EmitterHandle hit = kNullEmitterHandle;
    emitters_->ForEach([&](EmitterHandle h, EmitterRecord& rec) {
        if (rec.predictionId == predictionId && hit == kNullEmitterHandle) {
            hit = h;
        }
    });

    if (hit == kNullEmitterHandle) {
        // Nothing to cancel — the prediction either retired naturally,
        // was never started, or was evicted. From the host's
        // perspective, "cancelled" and "wasn't there" are the same
        // outcome, so we report success.
        ++statsLatest_.predictionsCancelledNotFound;
        return AudioResult::Success;
    }

    if (auto* rec = emitters_->Get(hit)) {
        StopMixerAndResetStreamingFor(*rec, std::max(0.0f, fadeOutMs));
        // Clear the prediction id so a subsequent stale Cancel won't
        // re-target this slot during the fade-out window. The slot
        // itself stays alive until TickOneShots frees it so the fade
        // can complete.
        rec->predictionId = 0;
        ++statsLatest_.predictionsCancelled;
    }
    return AudioResult::Success;
}

bool AudioRuntimeImpl::GetVoiceNetworkStats(AudioPlayerId playerId,
                                              AudioRuntime::VoiceNetworkStats& out) const {
    if (!initialized_ || !voices_) return false;
    const auto* s = voices_->GetVoiceStats(playerId);
    if (!s) return false;
    out.packetsReceived         = s->packetsReceived;
    out.packetsAccepted         = s->packetsAccepted;
    out.packetsLate             = s->packetsLate;
    out.packetsDuplicate        = s->packetsDuplicate;
    out.packetsReordered        = s->packetsReordered;
    out.packetsLost             = s->packetsLost;
    out.packetsOverwritten      = s->packetsOverwritten;
    out.packetsRateLimited      = s->packetsRateLimited;
    out.plcGenerated            = s->plcGenerated;
    out.silentFrames            = s->silentFrames;
    out.observedJitterMs        = s->currentObservedJitterMs;
    out.targetBufferDepthFrames = s->currentTargetDepth;
    return true;
}

AudioResult AudioRuntimeImpl::SetBusGainDb(BusId busId, float gainDb) {
    if (!initialized_) return AudioResult::NotInitialized;
    if (!busGraph_)    return AudioResult::NotInitialized;
    if (busGraph_->IndexOf(busId) == BusGraph::kInvalidIndex) {
        return AudioResult::InvalidArgument;
    }
    MixerCommand cmd;
    cmd.kind       = MixerCommandKind::SetBusGain;
    cmd.busId      = busId;
    cmd.paramValue = gainDb;
    return mixer_->PostCommand(cmd) ? AudioResult::Success : AudioResult::QueueFull;
}

BusId AudioRuntimeImpl::FindBusIdByName(std::string_view name) const {
    // Walk config_.busGraph.buses[]. We only honor entries up to
    // busCount; the rest are default-initialized junk. Compare against
    // the trailing-NUL-terminated debugName field, but accept matches
    // even when the input has no NUL (string_view) by length-checking
    // first.
    if (!initialized_) return kInvalidBusId;
    const auto& g = config_.busGraph;
    for (uint32_t i = 0; i < g.busCount; ++i) {
        const char* dn = g.buses[i].debugName;
        // Walk dn until NUL or sizeof; compare length-aware.
        size_t dnLen = 0;
        while (dnLen < sizeof(g.buses[i].debugName) && dn[dnLen] != '\0') ++dnLen;
        if (dnLen == name.size() &&
            std::memcmp(dn, name.data(), dnLen) == 0) {
            return g.buses[i].id;
        }
    }
    return kInvalidBusId;
}

AudioResult AudioRuntimeImpl::SetEffectParameter(BusId    busId,
                                                   uint32_t effectIndex,
                                                   uint16_t paramId,
                                                   float    value) {
    if (!initialized_) return AudioResult::NotInitialized;
    if (!busGraph_)    return AudioResult::NotInitialized;
    const uint32_t bi = busGraph_->IndexOf(busId);
    if (bi == BusGraph::kInvalidIndex) return AudioResult::InvalidArgument;
    if (effectIndex >= busGraph_->EffectCount(bi)) return AudioResult::InvalidArgument;
    MixerCommand cmd;
    cmd.kind        = MixerCommandKind::SetEffectParameter;
    cmd.busId       = busId;
    cmd.effectIndex = effectIndex;
    cmd.paramId     = paramId;
    cmd.paramValue  = value;
    return mixer_->PostCommand(cmd) ? AudioResult::Success : AudioResult::QueueFull;
}

Result<VoiceSourceHandle> AudioRuntimeImpl::RegisterVoiceSource(AudioPlayerId playerId) {
    if (!initialized_) return AudioResult::NotInitialized;
    auto r = voices_->Register(playerId);
    if (!r) return r.error();

    if (auto* rec = voices_->Get(r.value())) {
        // Bind voice source to a mixer voice immediately. Mixer keeps the
        // ring pointer alive as long as the voice source record lives;
        // Unregister sends Stop before freeing.
        MixerCommand cmd;
        cmd.kind          = MixerCommandKind::StartVoice;
        cmd.mixSlot       = rec->mixSlot;
        cmd.gain          = 1.0f;
        cmd.pan           = 0.0f;
        cmd.voiceRing     = rec->pcmRing.get();
        cmd.voiceChannels = 1;
        mixer_->PostCommand(cmd);
        rec->mixerStarted = true;
    }
    return r;
}

AudioResult AudioRuntimeImpl::UnregisterVoiceSource(VoiceSourceHandle h) {
    if (!initialized_) return AudioResult::NotInitialized;
    if (auto* rec = voices_->Get(h); rec && rec->mixerStarted) {
        PostMixerStopForEmitter(rec->mixSlot);
    }
    return voices_->Unregister(h);
}

// ---- 2.4 mute / volume ----------------------------------------------------

AudioResult AudioRuntimeImpl::SetVoiceSourceMuted(AudioPlayerId p, bool muted) {
    if (!initialized_) return AudioResult::NotInitialized;
    return voices_->SetMuted(p, muted);
}
AudioResult AudioRuntimeImpl::SetVoiceSourceVolume(AudioPlayerId p, float vol) {
    if (!initialized_) return AudioResult::NotInitialized;
    return voices_->SetVolume(p, vol);
}
bool AudioRuntimeImpl::IsVoiceSourceMuted(AudioPlayerId p, bool& out) const {
    if (!initialized_ || !voices_) return false;
    return voices_->GetMuted(p, out);
}
bool AudioRuntimeImpl::GetVoiceSourceVolume(AudioPlayerId p, float& out) const {
    if (!initialized_ || !voices_) return false;
    return voices_->GetVolume(p, out);
}

// ---- 2.6 bandwidth budget --------------------------------------------------

AudioResult AudioRuntimeImpl::SetVoiceBandwidthBudget(
        AudioPlayerId p, uint32_t bytesPerSec) {
    if (!initialized_) return AudioResult::NotInitialized;
    return voices_->SetBandwidthBudget(p, bytesPerSec);
}
int32_t AudioRuntimeImpl::SuggestVoiceBitrate(
        AudioPlayerId p, uint32_t frameMs) {
    if (!initialized_ || !voices_) return 32000;
    return voices_->SuggestBitrate(p, frameMs);
}
AudioResult AudioRuntimeImpl::ReportVoiceBytesSent(
        AudioPlayerId p, uint32_t bytes, int32_t bitrateUsedBps) {
    if (!initialized_) return AudioResult::NotInitialized;
    return voices_->ReportBytesSent(p, bytes, bitrateUsedBps);
}

// ---- Network thread API ---------------------------------------------------

void AudioRuntimeImpl::OnTickAdvanced(SimulationTick tick, TimestampMs serverTimeMs) {
    if (!initialized_) return;
    const TimestampMs prev = latestServerTimeMs_.load(std::memory_order_acquire);
    previousServerTimeMs_.store(prev, std::memory_order_release);
    latestServerTimeMs_.store(serverTimeMs, std::memory_order_release);
    latestNetworkTick_.store(tick,         std::memory_order_release);

    // Reset the rate limiter's per-tick new-player admission counter
    // so a fresh budget is available for legitimate joins this tick.
    replicationRateLimiter_.OnTickAdvanced(tick);
}

AudioResult AudioRuntimeImpl::SubmitReplicatedEvent(const AudioEvent& event) {
    // Backward-compatible 1-arg form. Source = Unknown means the
    // runtime treats the event permissively — no Phase 2.5
    // server-authoritative enforcement, but the validator hook and
    // rate limiter still apply.
    return SubmitReplicatedEvent(event, ReplicationSource::Unknown,
                                  EventDelivery::Drop);
}

AudioResult AudioRuntimeImpl::SubmitReplicatedEvent(const AudioEvent& event,
                                                     ReplicationSource source) {
    // Backward-compatible 2-arg form. Defaults to Drop, which matches
    // the pre-v0.18.0 behavior (late events past the staleness
    // threshold are discarded).
    return SubmitReplicatedEvent(event, source, EventDelivery::Drop);
}

AudioResult AudioRuntimeImpl::SubmitReplicatedEvent(const AudioEvent& event,
                                                     ReplicationSource source,
                                                     EventDelivery     delivery) {
    if (!initialized_) return AudioResult::NotInitialized;

    // Step 0: stamp the delivery class onto the event for the
    // control-thread Phase-2 drain to consult. The 1-arg and 2-arg
    // overloads pass Drop here; explicit-classification callers can
    // override. We mutate a local copy rather than the caller's
    // event so the API doesn't silently overwrite a field the host
    // had set differently.
    AudioEvent stamped = event;
    stamped.delivery = delivery;

    // Step 1: replication-policy enforcement (Phase 2.5).
    //
    // A Client-sourced event declaring ServerAuthoritative policy is
    // a spoof attempt — only the server is allowed to author state
    // changes that take effect on remote listeners. Reject before
    // the validator/rate-limiter and bump the dedicated
    // policyViolations counter (separate from the validator hook's
    // counter) so dashboards can distinguish "runtime caught a
    // protocol-level spoof" from "host's custom validator denied."
    if (source == ReplicationSource::Client &&
        stamped.replicationPolicy == AudioReplicationPolicy::ServerAuthoritative) {
        replicationRateLimiter_.RecordPolicyViolation(stamped.playerId);
        if (ShouldLog_(LogLevel::Warn)) {
            const LogField fields[] = {
                LogField::UInt("player_id",  static_cast<uint64_t>(stamped.playerId)),
                LogField::UInt("sound_id",   static_cast<uint64_t>(stamped.soundId)),
                LogField::UInt("category",   static_cast<uint64_t>(stamped.category)),
                LogField::Str ("reason",     "client_sourced_server_authoritative"),
            };
            Log_(static_cast<uint8_t>(LogLevel::Warn),
                  LogCategory::kReplication,
                  "replication policy violation rejected",
                  fields);
        }
        return AudioResult::PolicyViolation;
    }

    // Step 2: host policy hook. If the host has installed a validator
    // and it rejects, drop silently and bump the rejection counter.
    if (replicationValidator_ != nullptr &&
        !replicationValidator_->ShouldAccept(stamped, stamped.playerId)) {
        replicationRateLimiter_.RecordValidatorRejection(stamped.playerId);
        if (ShouldLog_(LogLevel::Debug)) {
            const LogField fields[] = {
                LogField::UInt("player_id",  static_cast<uint64_t>(stamped.playerId)),
                LogField::UInt("sound_id",   static_cast<uint64_t>(stamped.soundId)),
                LogField::UInt("category",   static_cast<uint64_t>(stamped.category)),
            };
            Log_(static_cast<uint8_t>(LogLevel::Debug),
                  LogCategory::kReplication,
                  "replication event rejected by host validator",
                  fields);
        }
        return AudioResult::PolicyViolation;
    }

    // Step 3: per-player, per-category token-bucket rate limit.
    // Clock is the most recent serverTimeMs from OnTickAdvanced for
    // determinism; if no tick has advanced yet, the bucket is
    // pre-filled to capacity so the host's first burst is accepted.
    const TimestampMs nowMs =
        latestServerTimeMs_.load(std::memory_order_acquire);
    if (!replicationRateLimiter_.TryAccept(stamped.playerId,
                                            stamped.category,
                                            nowMs)) {
        if (ShouldLog_(LogLevel::Debug)) {
            const LogField fields[] = {
                LogField::UInt("player_id",  static_cast<uint64_t>(stamped.playerId)),
                LogField::UInt("category",   static_cast<uint64_t>(stamped.category)),
                LogField::UInt("now_ms",     static_cast<uint64_t>(nowMs)),
            };
            Log_(static_cast<uint8_t>(LogLevel::Debug),
                  LogCategory::kReplication,
                  "replication event rate-limited",
                  fields);
        }
        return AudioResult::RateLimited;
    }

    // Step 4: classification telemetry. Counted at submission, not
    // at drain — host operators want "events you tried to send"
    // bucketed by class, not "events that passed the rate limiter."
    // Bumped under the network thread; reads from the control
    // thread are racy by construction (counters are uint64_t, not
    // atomic — accepting torn reads is fine for monotonic counters
    // displayed in dashboards).
    if (delivery == EventDelivery::Guaranteed) {
        ++statsLatest_.eventsSubmittedGuaranteed;
    } else {
        ++statsLatest_.eventsSubmittedDrop;
    }

    // Step 5: enqueue for the control thread.
    return netEvents_->Push(stamped) ? AudioResult::Success : AudioResult::QueueFull;
}

AudioResult AudioRuntimeImpl::UpdateReplicatedTransform(EmitterHandle  h,
                                                          const Vec3&    pos,
                                                          const Vec3&    fwd,
                                                          const Vec3&    vel,
                                                          SimulationTick tick) {
    // Backward-compatible 5-arg form. Routes through the mask
    // overload with mask=All; pre-v0.18.0 callers see no behavior
    // change.
    return UpdateReplicatedTransform(h, TransformStateMask::All,
                                       pos, fwd, vel, tick);
}

AudioResult AudioRuntimeImpl::UpdateReplicatedTransform(EmitterHandle      h,
                                                          TransformStateMask mask,
                                                          const Vec3&        pos,
                                                          const Vec3&        fwd,
                                                          const Vec3&        vel,
                                                          SimulationTick     tick) {
    if (!initialized_) return AudioResult::NotInitialized;
    // No-op fast path: a mask with no bits set is a host bug, but
    // returning Success without enqueuing is the cheapest correct
    // response (we don't want to fill the ring with empty updates,
    // and InvalidArgument would force every caller to check —
    // failures should be returned for things the host can fix).
    if (mask == TransformStateMask::None) {
        return AudioResult::Success;
    }
    ReplicatedTransformUpdate u;
    u.handle   = h;
    u.position = pos;
    u.forward  = fwd;
    u.velocity = vel;
    u.tick     = tick;
    u.mask     = mask;
    return netTransforms_->Push(u) ? AudioResult::Success : AudioResult::QueueFull;
}

AudioResult AudioRuntimeImpl::OnVoicePacket(AudioPlayerId  playerId,
                                              const uint8_t* bytes,
                                              size_t         size,
                                              uint16_t       sequenceNumber,
                                              TimestampMs    timestampMs) {
    // Sample steady_clock here so the deterministic overload below
    // is the single source of truth for the actual ingest logic.
    // Hosts that want bit-identical replay use the 6-arg form
    // directly with their own tick clock.
    const TimestampMs arrivalMs = static_cast<TimestampMs>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return OnVoicePacket(playerId, bytes, size, sequenceNumber,
                          timestampMs, arrivalMs);
}

AudioResult AudioRuntimeImpl::OnVoicePacket(AudioPlayerId  playerId,
                                              const uint8_t* bytes,
                                              size_t         size,
                                              uint16_t       sequenceNumber,
                                              TimestampMs    timestampMs,
                                              TimestampMs    arrivalMs) {
    if (!initialized_) return AudioResult::NotInitialized;
    if (!bytes || size == 0)              return AudioResult::InvalidArgument;
    if (size > kMaxVoicePacketBytes)      return AudioResult::BudgetExceeded;
    if (size > config_.voiceMaxPacketBytes) return AudioResult::BudgetExceeded;

    // Per-player Voice category rate limiting. Rejecting here saves
    // the decode cost downstream and protects the SPSC ring from
    // floods. Same deterministic clock (`latestServerTimeMs_`) used
    // for replicated events; same per-tick new-player budget gates
    // playerId-cycling DoS attempts on the voice path.
    const TimestampMs nowMs =
        latestServerTimeMs_.load(std::memory_order_acquire);
    if (!replicationRateLimiter_.TryAccept(playerId,
                                            AudioCategory::Voice,
                                            nowMs)) {
        // Surface in per-player voice telemetry too. No-op if the
        // player has no registered voice source.
        if (voices_) voices_->BumpVoicePacketRateLimited(playerId);
        return AudioResult::RateLimited;
    }

    VoicePacketCopy copy;
    copy.playerId       = playerId;
    copy.sequenceNumber = sequenceNumber;
    copy.size           = static_cast<uint16_t>(size);
    copy.timestampMs    = timestampMs;
    copy.arrivalMs      = arrivalMs;
    std::memcpy(copy.bytes.data(), bytes, size);
    return voicePackets_->Push(std::move(copy)) ? AudioResult::Success : AudioResult::QueueFull;
}

// ---- Helpers --------------------------------------------------------------

bool AudioRuntimeImpl::IsEventTooLate(TimestampMs eventMs, TimestampMs nowMs) const {
    return IsEventTooLateWithOverride(eventMs, nowMs, /*overrideMs*/ 0);
}

bool AudioRuntimeImpl::IsEventTooLateWithOverride(TimestampMs eventMs,
                                                    TimestampMs nowMs,
                                                    uint32_t    overrideMs) const {
    if (eventMs == 0 || nowMs == 0) return false;     // unset timestamps: not late
    if (nowMs <= eventMs)            return false;
    const uint32_t threshold = (overrideMs > 0) ? overrideMs : config_.lateEventDiscardMs;
    return (nowMs - eventMs) > threshold;
}

void AudioRuntimeImpl::PostMixerStartForEmitter(EmitterRecord& rec, const PcmAsset& asset) {
    MixerCommand cmd;
    cmd.kind        = MixerCommandKind::StartSound;
    cmd.mixSlot     = rec.assignedMixSlot;
    cmd.gain        = rec.gainParam;
    cmd.pan         = 0.0f;
    cmd.pitch       = rec.pitchParam;
    cmd.targetBus   = ResolveBusForEmitter(rec);
    cmd.pcmData     = asset.samples.data();
    cmd.pcmFrames   = asset.frames;
    cmd.pcmChannels = asset.channels;
    cmd.looping     = rec.descriptor.isLooping;
    cmd.fadeInMs    = rec.descriptor.fadeInMs;
    // Resolve loop-boundary crossfade from the sound definition. The
    // value is in milliseconds; the mixer wants frames. Clamp to less
    // than half the buffer (validated by the mixer too).
    if (cmd.looping) {
        if (const auto* def = assets_->GetDefinition(rec.descriptor.soundId)) {
            const float ms = std::max(0.0f, def->loopCrossfadeMs);
            const uint32_t requested = static_cast<uint32_t>(
                (ms / 1000.0f) * static_cast<float>(config_.sampleRate));
            const uint32_t cap = asset.frames > 1 ? (asset.frames - 1) / 2 : 0u;
            cmd.loopXfadeFrames = std::min(requested, cap);
        }
    }
    if (mixer_->PostCommand(cmd)) {
        rec.mixerStarted          = true;
        rec.activeSoundId         = rec.descriptor.soundId;
        rec.oneShotFramesRemaining = rec.descriptor.isLooping
            ? 0.0
            : static_cast<double>(asset.frames);
    }
}

void AudioRuntimeImpl::PostMixerStartStreamingForEmitter(EmitterRecord&  rec,
                                                          StreamingAsset& asset) {
    if (!asset.ring || !asset.decoder) return;

    // Rewind and pre-roll the ring before posting the start command. Pre-
    // rolling avoids an immediate underrun on the first render callback;
    // the pump will otherwise only top up next tick.
    asset.ring->Reset();
    asset.decoder->Seek(0);

    const uint32_t prerollFrames = std::min(
        asset.ring->FreeFrames(),
        config_.streamingDecodeChunkFrames);
    if (prerollFrames > 0) {
        const uint32_t got = asset.decoder->DecodeFrames(
            streamingDecodeScratch_.data(), prerollFrames);
        if (got > 0) asset.ring->Push(streamingDecodeScratch_.data(), got);
    }

    MixerCommand cmd;
    cmd.kind           = MixerCommandKind::StartStreamingSound;
    cmd.mixSlot        = rec.assignedMixSlot;
    cmd.gain           = rec.gainParam;
    cmd.pan            = 0.0f;
    cmd.pitch          = 1.0f;
    cmd.targetBus      = ResolveBusForEmitter(rec);
    cmd.streamRing     = asset.ring.get();
    cmd.streamChannels = asset.channels;
    cmd.looping        = rec.descriptor.isLooping;
    cmd.fadeInMs       = rec.descriptor.fadeInMs;
    if (mixer_->PostCommand(cmd)) {
        rec.mixerStarted   = true;
        rec.activeSoundId  = rec.descriptor.soundId;
        // Looping streaming runs until DestroyEmitter; non-looping uses
        // the existing one-shot tick mechanism, sized by totalFrames if
        // the decoder reports it. Unknown-length streams (totalFrames == 0)
        // are kept alive until the host calls DestroyEmitter, since the
        // pump can't predict EOF in advance.
        rec.oneShotFramesRemaining =
            rec.descriptor.isLooping || asset.totalFrames == 0
                ? 0.0
                : static_cast<double>(asset.totalFrames);
        asset.state          = StreamingAsset::State::Pumping;
        asset.playingMixSlot = rec.assignedMixSlot;
    }
}

BusId AudioRuntimeImpl::ResolveBusForEmitter(const EmitterRecord& rec) const noexcept {
    if (rec.descriptor.targetBus != kInvalidBusId) return rec.descriptor.targetBus;
    const CategoryBusMap& m = config_.busGraph.categoryMap;
    switch (rec.descriptor.category) {
        case AudioCategory::Music:    return m.music;
        case AudioCategory::Voice:    return m.voice;
        case AudioCategory::SFX:      return m.sfx;
        case AudioCategory::Ambience: return m.ambience;
        case AudioCategory::UI:       return m.ui;
        case AudioCategory::Dialogue: return m.dialogue;
        case AudioCategory::Count:    break;
    }
    return kBusMaster;
}

void AudioRuntimeImpl::PostMixerStopForEmitter(uint32_t mixSlot, float fadeOutMs) {
    MixerCommand cmd;
    cmd.kind       = MixerCommandKind::Stop;
    cmd.mixSlot    = mixSlot;
    cmd.fadeOutMs  = fadeOutMs;
    mixer_->PostCommand(cmd);
}

void AudioRuntimeImpl::StopMixerAndResetStreamingFor(const EmitterRecord& rec, float fadeOutMs) {
    if (auto* stream = assets_->GetStreamingAsset(rec.activeSoundId)) {
        stream->state = StreamingAsset::State::Idle;
        if (stream->ring)    stream->ring->Reset();
        if (stream->decoder) stream->decoder->Seek(0);
    }
    if (rec.mixerStarted) PostMixerStopForEmitter(rec.assignedMixSlot, fadeOutMs);
}

int64_t AudioRuntimeImpl::EffectivePriorityForCandidate(AudioPriority pri,
                                                          const Vec3& pos,
                                                          const Vec3& listenerPos) const noexcept {
    // Encode priority in the high bits and (negated) distance in the low
    // bits so that priority always trumps distance, with closer = higher
    // effective priority among ties. Distance is clamped to a wide but
    // finite window so the tie-breaker never underflows past the next
    // priority bucket.
    const int64_t pBits = static_cast<int64_t>(static_cast<uint8_t>(pri)) << 32;
    const float   dx    = pos.x - listenerPos.x;
    const float   dy    = pos.y - listenerPos.y;
    const float   dz    = pos.z - listenerPos.z;
    const float   d2    = dx*dx + dy*dy + dz*dz;
    // Quantize sqrt(d2) into millimetres, clamp to ~ 1e6 mm = 1 km so the
    // tie-breaker stays in the low-32-bit window.
    const double  dMm   = static_cast<double>(std::sqrt(d2)) * 1000.0;
    const int64_t dInt  = static_cast<int64_t>(
        std::min<double>(std::max<double>(dMm, 0.0), 1.0e9));
    return pBits - dInt;
}

int64_t AudioRuntimeImpl::EffectivePriority(const EmitterRecord& rec,
                                              const Vec3& listenerPos) const noexcept {
    // Persistent (non-oneShot) emitters are owned by the host and are not
    // valid eviction targets; return a sentinel that wins every comparison.
    if (!rec.oneShot) return INT64_MAX;
    return EffectivePriorityForCandidate(rec.descriptor.priority,
                                           rec.position,
                                           listenerPos);
}

bool AudioRuntimeImpl::EvictLowestPriorityOneShotIfBeatenBy(int64_t incoming) {
    if (!emitters_) return false;
    const Vec3 listenerPos = listeners_->HasPrimary()
        ? listeners_->Primary().position
        : Vec3{};

    EmitterHandle victim{};
    int64_t       victimPri    = INT64_MAX;
    bool          haveVictim   = false;

    emitters_->ForEach([&](EmitterHandle h, EmitterRecord& rec) {
        if (!rec.oneShot) return;          // skip persistent emitters
        const int64_t pri = EffectivePriority(rec, listenerPos);
        if (!haveVictim || pri < victimPri) {
            victim     = h;
            victimPri  = pri;
            haveVictim = true;
        }
    });

    if (!haveVictim || victimPri >= incoming) return false;

    // Evict: post a 20 ms-faded Stop to the mixer, drop smoother state,
    // free the slot. The fade is best-effort: a fresh StartSound on the
    // same slot (which happens when the eviction is followed
    // immediately by the incoming sound's PostMixerStartForEmitter)
    // preempts the fade. For non-eviction stops the fade plays out.
    if (auto* rec = emitters_->Get(victim)) {
        StopMixerAndResetStreamingFor(*rec, /*fadeOutMs=*/20.0f);
    }
    if (orchestrator_) orchestrator_->Smoother().Forget(victim);
    emitters_->Destroy(victim);
    ++statsLatest_.oneShotEvictions;
    if (ShouldLog_(LogLevel::Debug)) {
        const LogField fields[] = {
            LogField::UInt("victim_handle", static_cast<uint64_t>(victim.index)),
            LogField::UInt("victim_generation",
                            static_cast<uint64_t>(victim.generation)),
            LogField::Int ("incoming_priority", incoming),
        };
        Log_(static_cast<uint8_t>(LogLevel::Debug),
              LogCategory::kEmitter,
              "one-shot evicted: lower-priority slot freed for incoming",
              fields);
    }
    return true;
}

void AudioRuntimeImpl::PumpStreamingAssets() {
    // Bounded work: each asset takes at most one decode chunk per tick.
    // Idle assets are skipped; Draining assets just wait for the ring to
    // drain (the mixer drains it; the one-shot tick destroys the emitter
    // when oneShotFramesRemaining hits zero).
    const uint32_t chunkFrames = config_.streamingDecodeChunkFrames;
    if (chunkFrames == 0) return;

    assets_->ForEachStreamingAsset(
        [this, chunkFrames](AudioSoundId, StreamingAsset& asset) {
            if (asset.state != StreamingAsset::State::Pumping) return;
            if (!asset.decoder || !asset.ring) return;

            const uint32_t free = asset.ring->FreeFrames();
            if (free < chunkFrames) return;     // ring still has plenty of headroom

            const uint32_t want = std::min(free, chunkFrames);
            const uint32_t got  = asset.decoder->DecodeFrames(
                streamingDecodeScratch_.data(), want);

            if (got > 0) asset.ring->Push(streamingDecodeScratch_.data(), got);

            if (got < want) {
                // Decoder hit EOF while we still wanted more data.
                if (asset.looping) {
                    // Loop: rewind and immediately top up the rest.
                    asset.decoder->Seek(0);
                    const uint32_t need = want - got;
                    const uint32_t got2 = asset.decoder->DecodeFrames(
                        streamingDecodeScratch_.data(), need);
                    if (got2 > 0) asset.ring->Push(streamingDecodeScratch_.data(), got2);
                } else {
                    // Non-looping: stop pumping. Mixer will drain the ring;
                    // the one-shot lifetime tick (which was sized to
                    // totalFrames at start) handles emitter teardown.
                    asset.state = StreamingAsset::State::Draining;
                }
            }
        });
}

void AudioRuntimeImpl::StartOneShotForSound(AudioSoundId soundId,
                                              const Vec3& pos,
                                              AudioPriority pri,
                                              uint64_t predictionId) {
    StreamingAsset* stream = assets_->GetStreamingAsset(soundId);
    const PcmAsset* asset  = stream ? nullptr : assets_->GetAsset(soundId);
    if (!stream && !asset) return;     // missing asset: silent drop

    // Streaming asset already in use? Constraint: one playing instance per
    // streaming asset. Drop the request silently to mirror the StartSound
    // event surface (which has no return channel for budget errors).
    if (stream && stream->state != StreamingAsset::State::Idle) return;

    const auto* def = assets_->GetDefinition(soundId);

    EmitterDescriptor desc;
    desc.soundId           = soundId;
    desc.position          = pos;
    desc.priority          = pri;
    desc.isLooping         = def ? def->looping : false;
    desc.isSpatialized     = def ? def->spatialized : true;
    desc.occlusionEnabled  = def ? def->occlusionEnabled : true;
    desc.attenuation       = def ? def->attenuation : AttenuationSettings{};
    desc.category          = def ? def->category : AudioCategory::SFX;
    desc.targetBus         = def ? def->targetBus : kInvalidBusId;

    // Try to allocate a slot. If the pool is full, run priority-eviction:
    // find the lowest-effective-priority playing one-shot and, if its
    // effective priority is strictly lower than the incoming sound's,
    // evict it and retry. Persistent (CreateEmitter) emitters are immune
    //; they're host-owned. If no eviction candidate beats us, drop the
    // request and bump the dropped-full-pool stat.
    auto h = emitters_->Create(desc, /*oneShot=*/!desc.isLooping);
    if (!h) {
        const Vec3 listenerPos = listeners_->HasPrimary()
            ? listeners_->Primary().position
            : Vec3{};
        const int64_t incoming =
            EffectivePriorityForCandidate(pri, pos, listenerPos);
        if (!EvictLowestPriorityOneShotIfBeatenBy(incoming)) {
            ++statsLatest_.oneShotsDroppedFullPool;
            if (ShouldLog_(LogLevel::Debug)) {
                const LogField fields[] = {
                    LogField::UInt("sound_id",  static_cast<uint64_t>(soundId)),
                    LogField::UInt("priority",  static_cast<uint64_t>(pri)),
                    LogField::Str ("reason",    "no_eviction_candidate"),
                };
                Log_(static_cast<uint8_t>(LogLevel::Debug),
                      LogCategory::kEmitter,
                      "one-shot dropped: pool full and no lower-priority candidate",
                      fields);
            }
            return;
        }
        h = emitters_->Create(desc, /*oneShot=*/!desc.isLooping);
        if (!h) {
            // Should not happen; eviction freed a slot. Defensive.
            ++statsLatest_.oneShotsDroppedFullPool;
            if (ShouldLog_(LogLevel::Warn)) {
                const LogField fields[] = {
                    LogField::UInt("sound_id",  static_cast<uint64_t>(soundId)),
                    LogField::Str ("reason",    "post_eviction_create_failed"),
                };
                Log_(static_cast<uint8_t>(LogLevel::Warn),
                      LogCategory::kEmitter,
                      "one-shot dropped: emitter create failed after eviction",
                      fields);
            }
            return;
        }
    }

    if (auto* rec = emitters_->Get(h.value())) {
        rec->predictionId = predictionId;
        if (stream) {
            stream->looping = desc.isLooping;
            PostMixerStartStreamingForEmitter(*rec, *stream);
        } else {
            PostMixerStartForEmitter(*rec, *asset);
        }
    }
}

void AudioRuntimeImpl::HandleEvent(const AudioEvent& e, bool /*replicated*/) {
    switch (e.type) {
        case AudioEventType::PlaySoundAtLocation:
            StartOneShotForSound(e.soundId, e.position, e.priority, e.predictionId);
            break;
        case AudioEventType::PlaySoundAttachedToActor:
            // Actor-to-position resolution is the host's responsibility;
            // we play at the carried position. Host-attached emitters that
            // need follow-the-actor semantics use CreateEmitter +
            // SetEmitterTransform (state model).
            StartOneShotForSound(e.soundId, e.position, e.priority, e.predictionId);
            break;
        case AudioEventType::StopEmitter:
            if (auto* rec = emitters_->Get(e.emitter)) {
                if (rec->mixerStarted) StopMixerAndResetStreamingFor(*rec);
                emitters_->Destroy(e.emitter);
            }
            break;
        case AudioEventType::SetEmitterParameter:
            orchestrator_->Smoother().SetTarget(
                e.emitter, e.parameterId, e.parameterValue, e.parameterSmoothingMs);
            break;
        case AudioEventType::SetEmitterTransform:
            emitters_->SetTransform(e.emitter, e.position, e.forward, e.velocity);
            break;
        case AudioEventType::TriggerSequence:
            orchestrator_->Sequencer().Trigger(e.sequenceId);
            break;
        case AudioEventType::UpdateReplicatedTransform:
            // Network seam normally uses UpdateReplicatedTransform directly,
            // not events. If routed through events, treat as transform set.
            emitters_->RecordReplicatedTransform(
                e.emitter, e.position, e.forward, e.velocity, e.simulationTick);
            break;
        case AudioEventType::RegisterVoiceSource:
        case AudioEventType::UnregisterVoiceSource:
        case AudioEventType::SetAudioZone:
        case AudioEventType::SetGameState:
            // Not handled via the event ring; the host calls the direct API.
            break;
    }
}

// ---- The 11-step Update flow ----------------------------------------------

// Public Update entry point. noexcept wrapper around UpdateBody_ that
// converts any escaped exception into a telemetry counter + log line so
// the control thread can never propagate an exception into the host's
// game loop, even if a third-party callback (telemetry sink, log sink,
// backend driver, decoder) misbehaves. v0.15.0 hardening.
//
// The two catch arms exist because the standard exception hierarchy
// is the common case but third-party C++ code is permitted to throw
// arbitrary objects (POD ints, custom types not deriving from
// std::exception). The catch(...) covers that long tail without
// allowing termination.
void AudioRuntimeImpl::Update(float deltaSeconds) noexcept {
    try {
        UpdateBody_(deltaSeconds);
    } catch (const std::exception& e) {
        ++statsLatest_.controlThreadExceptionsCaught;
        // Reuse the existing logging path. e.what() is a borrowed
        // pointer into the caught exception; the Log_ call copies on
        // the sink side so the lifetime extends correctly.
        Log_(static_cast<uint8_t>(LogLevel::Error),
              std::string_view("runtime"),
              std::string_view(e.what()),
              std::span<const LogField>{});
    } catch (...) {
        ++statsLatest_.controlThreadExceptionsCaught;
        Log_(static_cast<uint8_t>(LogLevel::Error),
              std::string_view("runtime"),
              std::string_view("AudioRuntime::Update: caught non-std "
                               "exception (third-party callback threw an "
                               "object not derived from std::exception)"),
              std::span<const LogField>{});
    }
}

void AudioRuntimeImpl::UpdateBody_(float deltaSeconds) {
    if (!initialized_) return;

    statsLatest_.eventsDrainedLastTick       = 0;
    statsLatest_.lateEventsDiscardedLastTick = 0;

    // v0.16.0 — Tier-2 hardening. UpdateBody_ orchestrates a sequence
    // of phase helpers. The original 386-line monolith decomposed
    // naturally along the 12 numbered comment boundaries it already
    // had. Phase 1 produces a tick context (network snapshot
    // timestamps) that flows into phases 2, 3, 5; other phases read
    // only class members. Trivial single-call phases (5.5 RTPC,
    // 10b streaming, 12 telemetry) inline existing helpers directly.
    const UpdateTickContext ctx =
        Update_Phase1_SnapshotNetworkState_(deltaSeconds);
    Update_Phase2_DrainNetworkEvents_(ctx.nowMs);
    Update_Phase3_DrainGameEvents_(ctx.nowMs);
    Update_Phase4_ApplyReplicatedTransforms_();
    Update_Phase5_InterpolateTransforms_(deltaSeconds,
                                          ctx.latestSrv,
                                          ctx.prevSrv);
    EvaluateRtpcBindings_();                                  // 5.5
    Update_Phase6_TickOrchestrator_(deltaSeconds);
    Update_Phase7_BuildEmitterSnapshot_();
    Update_Phase8_RunOcclusion_(deltaSeconds);
    Update_Phase9_SpatializeEmitters_();
    Update_Phase10_DrainVoicePackets_();
    PumpStreamingAssets();                                    // 10b
    Update_Phase11_TickOneShotsAndPublishStats_(deltaSeconds);
    EmitTelemetry_(deltaSeconds);                              // 12
}

// Phase 1: snapshot network-thread published state (atomic loads of
// tick / server-timestamp / previous-server-timestamp), advance the
// control-thread wall clock, and resolve nowMs (server time when
// available, control clock otherwise). Returns the snapshot for
// downstream phases.
AudioRuntimeImpl::UpdateTickContext
AudioRuntimeImpl::Update_Phase1_SnapshotNetworkState_(float deltaSeconds) noexcept {
    // -------------------------------------------------------------------
    // 1. Snapshot network-thread published state.
    //    The network thread owns latestNetworkTick_/latestServerTimeMs_.
    //    We read once and use the snapshot for the rest of this tick.
    // -------------------------------------------------------------------
    [[maybe_unused]] const SimulationTick latestTick = latestNetworkTick_.load(std::memory_order_acquire);
    const TimestampMs latestSrv = latestServerTimeMs_.load(std::memory_order_acquire);
    const TimestampMs prevSrv   = previousServerTimeMs_.load(std::memory_order_acquire);

    // Advance the control-thread wall clock used for late-event discard.
    controlClockMs_ += static_cast<TimestampMs>(deltaSeconds * 1000.0f);
    const TimestampMs nowMs = (latestSrv != 0) ? latestSrv : controlClockMs_;

    return UpdateTickContext{latestSrv, prevSrv, nowMs};
}

// Phase 2: drain the network-thread event ring. Bounded by
// maxNetworkEventsPerFrame. Late events (older than maxStalenessMs
// vs nowMs) are counted and discarded; on-time events flow through
// HandleEvent(replicated=true).
void AudioRuntimeImpl::Update_Phase2_DrainNetworkEvents_(TimestampMs nowMs) noexcept {
    // -------------------------------------------------------------------
    // 2. Drain network event ring (replicated events).
    //    Apply late-event discard against latestServerTimeMs.
    //
    //    v0.18.0 Tier-A: the late-discard policy now consults the
    //    event's delivery class. EventDelivery::Drop is discarded
    //    when late (the current behavior); EventDelivery::Guaranteed
    //    is processed regardless of staleness, and a counter is
    //    bumped so host operators can see the gap between "the host
    //    sent it on time" and "the host's reliable channel got it
    //    through late." That gap is the operational signal — high
    //    `eventsAcceptedGuaranteedLate` in steady state means the
    //    reliable transport is slow or the classification is wrong.
    // -------------------------------------------------------------------
    {
        AudioEvent e;
        uint32_t budget = config_.budget.maxNetworkEventsPerFrame;
        while (budget-- > 0 && netEvents_->Pop(e)) {
            ++statsLatest_.eventsDrainedLastTick;
            const bool isLate =
                IsEventTooLateWithOverride(e.timestampMs, nowMs, e.maxStalenessMs);
            if (isLate) {
                if (e.delivery == EventDelivery::Guaranteed) {
                    // Accept-late path. Bumped so dashboards can show
                    // "Guaranteed-class events that arrived late but
                    // were processed anyway."
                    ++statsLatest_.eventsAcceptedGuaranteedLate;
                    // Fall through to HandleEvent — no continue.
                } else {
                    // Drop path. Bump the per-class counter so the
                    // operator can compare submitted-vs-dropped per
                    // class without subtracting other categories.
                    ++statsLatest_.lateEventsDiscardedLastTick;
                    ++statsLatest_.eventsLateDropped;
                    if (ShouldLog_(LogLevel::Debug)) {
                        const LogField fields[] = {
                            LogField::UInt("event_ts_ms", e.timestampMs),
                            LogField::UInt("now_ms",      nowMs),
                            LogField::UInt("max_staleness_ms",
                                            static_cast<uint64_t>(e.maxStalenessMs)),
                            LogField::Bool("replicated",  true),
                        };
                        Log_(static_cast<uint8_t>(LogLevel::Debug),
                              LogCategory::kEvents,
                              "late event discarded",
                              fields);
                    }
                    continue;
                }
            }
            HandleEvent(e, /*replicated=*/true);
        }
    }

}

// Phase 3: drain the game-thread event ring. Same late-event policy
// as phase 2 but for locally-submitted events; HandleEvent is called
// with replicated=false so the runtime does not re-publish.
void AudioRuntimeImpl::Update_Phase3_DrainGameEvents_(TimestampMs nowMs) noexcept {
    // -------------------------------------------------------------------
    // 3. Drain game event ring (local events).
    //    Same late-event policy.
    // -------------------------------------------------------------------
    {
        AudioEvent e;
        uint32_t budget = config_.budget.maxGameEventsPerFrame;
        while (budget-- > 0 && gameEvents_->Pop(e)) {
            ++statsLatest_.eventsDrainedLastTick;
            if (IsEventTooLateWithOverride(e.timestampMs, nowMs, e.maxStalenessMs)) {
                ++statsLatest_.lateEventsDiscardedLastTick;
                if (ShouldLog_(LogLevel::Debug)) {
                    const LogField fields[] = {
                        LogField::UInt("event_ts_ms", e.timestampMs),
                        LogField::UInt("now_ms",      nowMs),
                        LogField::UInt("max_staleness_ms",
                                        static_cast<uint64_t>(e.maxStalenessMs)),
                        LogField::Bool("replicated",  false),
                    };
                    Log_(static_cast<uint8_t>(LogLevel::Debug),
                          LogCategory::kEvents,
                          "late event discarded",
                          fields);
                }
                continue;
            }
            HandleEvent(e, /*replicated=*/false);
        }
    }

}

// Phase 4: apply replicated-transform writes from the network thread
// into EmitterManager's two-tick history. Bounded by maxActiveEmitters.
void AudioRuntimeImpl::Update_Phase4_ApplyReplicatedTransforms_() noexcept {
    // -------------------------------------------------------------------
    // 4. Apply network-thread state writes (replicated transforms).
    //    Drained into the EmitterManager's two-tick history.
    //
    //    v0.18.0 Tier-A: the per-subfield mask flows through to
    //    RecordReplicatedTransform so untouched components don't
    //    shift their history. Pre-v0.18.0 callers (5-arg form)
    //    enqueue with mask=All and get the previous behavior.
    // -------------------------------------------------------------------
    {
        ReplicatedTransformUpdate u;
        uint32_t budget = config_.budget.maxActiveEmitters;
        while (budget-- > 0 && netTransforms_->Pop(u)) {
            emitters_->RecordReplicatedTransform(
                u.handle, u.mask, u.position, u.forward, u.velocity, u.tick);
        }
    }

}

// Phase 5: compute interpolation alpha (host-time progress through
// the current server-tick interval) and interpolate every replicated
// emitter's transform between its previous and current tick samples.
void AudioRuntimeImpl::Update_Phase5_InterpolateTransforms_(
    float deltaSeconds, TimestampMs latestSrv, TimestampMs prevSrv) noexcept {
    // -------------------------------------------------------------------
    // 5. Compute interpolation alpha and interpolate replicated transforms.
    //    alpha tracks how far through the current tick interval we are,
    //    based on host clock vs server tick timestamps.
    // -------------------------------------------------------------------
    if (latestSrv > prevSrv) {
        const float dtSrv = static_cast<float>(latestSrv - prevSrv) * 0.001f;
        if (dtSrv > 1e-3f) ticksObservedDt_ = dtSrv;
        interpAlpha_ = 0.0f;     // restart at boundary
    } else {
        interpAlpha_ += deltaSeconds / std::max(1e-3f, ticksObservedDt_);
        if (interpAlpha_ > 1.0f) interpAlpha_ = 1.0f;
    }
    emitters_->InterpolateReplicatedTransforms(interpAlpha_);

}

// Phase 6: tick the orchestrator (parameter smoothing + sequence
// player). Sequence steps fire synthesized PlaySound events through
// StartOneShotForSound directly (no extra event-ring hop).
void AudioRuntimeImpl::Update_Phase6_TickOrchestrator_(float deltaSeconds) noexcept {
    // -------------------------------------------------------------------
    // 6. Tick orchestrator (parameter smoothing + sequence player).
    //    Sequence steps fire synthesized PlaySound events through
    //    StartOneShotForSound directly (no extra ring hop).
    // -------------------------------------------------------------------
    orchestrator_->Tick(deltaSeconds, [this](AudioSoundId sid) {
        StartOneShotForSound(sid, Vec3{}, AudioPriority::Normal);
    });

}

// Phase 7: build the SoA emitter snapshot consumed by the spatializer
// and the occlusion system. Single buffered walk; output arrays are
// pre-reserved at init.
void AudioRuntimeImpl::Update_Phase7_BuildEmitterSnapshot_() noexcept {
    // -------------------------------------------------------------------
    // 7. Build SoA snapshot of emitters (used by spatializer + occlusion).
    // -------------------------------------------------------------------
    emitters_->BuildSnapshot(emitterViews_, slotPositions_, slotOccupied_);

}

// Phase 8: run the occlusion system (budgeted raycasts + smoothing).
// Writes per-slot occlusionAmount + damping, mirrors them back into
// the spatial views so phase 9 reads them. Skipped when occlusion is
// disabled or no listener exists.
void AudioRuntimeImpl::Update_Phase8_RunOcclusion_(float deltaSeconds) noexcept {
    // -------------------------------------------------------------------
    // 8. Run occlusion system (budgeted raycasts + smoothing).
    //    Writes to occlusionAmounts_[slot]; we then mirror into the views.
    // -------------------------------------------------------------------
    if (config_.enableOcclusion && listeners_->HasPrimary()) {
        occlusion_->Update(listeners_->Primary().position,
                            slotPositions_.data(),
                            slotOccupied_.data(),
                            occlusionAmounts_.data(),
                            occlusionDamping_.data(),
                            deltaSeconds);
        statsLatest_.occlusionChecksLastTick = occlusion_->LastFrameRaycasts();
        // Mirror back into the views.
        for (size_t i = 0; i < emitterViews_.size(); ++i) {
            emitterViews_[i].occlusionAmount  = occlusionAmounts_[i];
            emitterViews_[i].occlusionDamping = occlusionDamping_[i];
        }
    } else {
        statsLatest_.occlusionChecksLastTick = 0;
    }

}

// Phase 9: per-emitter spatial computation. For each active
// mixerStarted emitter, compute spatial parameters via the
// spatializer and push UpdateParams to the mixer. Interest-managed:
// when more active emitters exist than the per-tick budget, the
// closest N to the listener are processed and the rest skipped
// (transforms still updated; only spatial-param recomputation is
// budgeted). Longest phase by a wide margin; will receive its own
// sub-decomposition in a later release if profiling shows extractable
// sub-phases.
void AudioRuntimeImpl::Update_Phase9_SpatializeEmitters_() noexcept {
    // -------------------------------------------------------------------
    // 9. For each emitter: compute spatial params, push UpdateParams to
    //    mixer. mixerStarted slots only; others haven't been started yet.
    // -------------------------------------------------------------------
    if (listeners_->HasPrimary()) {
        const SpatialListenerView listenerView = listeners_->BuildView();
        SpatialEnvironmentState env;
        env.dopplerEnabled        = config_.enableDoppler;
        env.globalReverbSend      = config_.globalReverbSend;
        env.globalLowPass         = 0.0f;
        env.speedOfSound          = config_.speedOfSound;
        env.airAbsorptionPerMeter = config_.enableAirAbsorption
            ? config_.airAbsorptionPerMeter
            : 0.0f;

        // Phase 1: collect every active emitter into the sort scratch
        // along with its squared distance to the listener. clear()
        // preserves capacity so we don't allocate per tick.
        interestSortScratch_.clear();
        emitters_->ForEach([&](EmitterHandle h, EmitterRecord& rec) {
            if (!rec.mixerStarted) return;
            const SpatialEmitterView& v = emitterViews_[h.index];
            const float dx = v.position.x - listenerView.position.x;
            const float dy = v.position.y - listenerView.position.y;
            const float dz = v.position.z - listenerView.position.z;
            interestSortScratch_.push_back({h, dx*dx + dy*dy + dz*dz, &rec});
        });

        const uint32_t budget   = config_.budget.maxActiveEmittersProcessedPerTick;
        const size_t   total    = interestSortScratch_.size();
        size_t         processN = total;

        if (budget > 0 && total > budget) {
            // Partition such that the first `budget` entries are the
            // closest by distance. nth_element is O(n) average and
            // doesn't fully sort either partition; we don't need
            // ordering, only "in vs out".
            std::nth_element(
                interestSortScratch_.begin(),
                interestSortScratch_.begin() + budget,
                interestSortScratch_.end(),
                [](const InterestSortEntry& a, const InterestSortEntry& b) {
                    return a.dist2 < b.dist2;
                });
            processN = budget;
        }
        statsLatest_.emittersProcessedLastTick      = static_cast<uint32_t>(processN);
        statsLatest_.emittersSkippedByInterestLastTick =
            static_cast<uint32_t>(total - processN);

        // Phase 2a: process the closest N. If any of these were muted
        // by interest on a previous tick, the fresh UpdateParams below
        // unmutes them by carrying real spatial values; flip the flag
        // so we don't keep posting unmute-as-update every tick.
        for (size_t i = 0; i < processN; ++i) {
            const InterestSortEntry& entry = interestSortScratch_[i];
            EmitterHandle  h   = entry.handle;
            EmitterRecord& rec = *entry.rec;

            const SpatialEmitterView& v = emitterViews_[h.index];
            SpatialParams sp = spatializer_->Calculate(v, listenerView, env);

            // Combine with smoothed user params.
            const float userGain  = orchestrator_->Smoother().Get(h, AudioParameterIds::Gain,  1.0f);
            const float userPitch = orchestrator_->Smoother().Get(h, AudioParameterIds::Pitch, 1.0f);
            sp.gain  *= userGain;
            sp.pitch *= userPitch;

            // RTPC-modulated low-pass and reverb-send. Combined with
            // the spatializer's outputs rather than overwriting them:
            //   * LowPass: max() so RTPC can add filtering on top of
            //     occlusion and air-absorption baseline, but never
            //     reduce filtering applied by the world.
            //   * ReverbSend: clamped sum so RTPC adds wetness on top
            //     of the global send.
            // Default fallback (0) is the identity for both combiners
            // — sounds without a binding behave exactly as before.
            const float userLpf    = orchestrator_->Smoother().Get(h, AudioParameterIds::LowPassAmount, 0.0f);
            const float userReverb = orchestrator_->Smoother().Get(h, AudioParameterIds::ReverbSend,    0.0f);
            if (userLpf > sp.lowPassAmount) sp.lowPassAmount = userLpf;
            sp.reverbSend = std::min(1.0f, sp.reverbSend + userReverb);

            // Apply per-bus proximity curve, if the emitter routes to a
            // bus that has one. This implements the silent-send pattern
            // (RemoteGunNearby) entirely on the control thread: we
            // compute distance to the listener, look up the bus's curve,
            // and scale the voice's gain accordingly. The mixer's
            // render path is unchanged.
            const BusId emitterBus = ResolveBusForEmitter(rec);
            if (const ProximityCurve* pc = busGraph_->ProximityCurveFor(emitterBus); pc != nullptr) {
                const float dx = v.position.x - listenerView.position.x;
                const float dy = v.position.y - listenerView.position.y;
                const float dz = v.position.z - listenerView.position.z;
                const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                float prox = 0.0f;
                if (dist <= pc->fullDistance) {
                    prox = 1.0f;
                } else if (dist >= pc->falloffDistance) {
                    prox = 0.0f;
                } else {
                    const float t   = (dist - pc->fullDistance)
                                    / (pc->falloffDistance - pc->fullDistance);
                    prox = std::pow(std::max(0.0f, 1.0f - t), pc->curveExponent);
                }
                sp.gain *= prox;
            }

            MixerCommand cmd;
            cmd.kind          = MixerCommandKind::UpdateParams;
            cmd.mixSlot       = rec.assignedMixSlot;
            cmd.gain          = sp.gain;
            cmd.pan           = sp.pan;
            cmd.pitch         = sp.pitch;
            cmd.lowPassAmount = sp.lowPassAmount;
            cmd.reverbSend    = sp.reverbSend;
            cmd.useBinaural   = sp.useBinaural;
            cmd.gainL         = sp.gainL;
            cmd.gainR         = sp.gainR;
            cmd.delaySamplesL = sp.delaySamplesL;
            cmd.delaySamplesR = sp.delaySamplesR;
            cmd.lpfAmountL    = sp.lpfAmountL;
            cmd.lpfAmountR    = sp.lpfAmountR;
            mixer_->PostCommand(cmd);

            rec.inInterestMute = false;
        }

        // Phase 2b: emitters outside the budget. Edge-trigger a single
        // zero-gain UpdateParams the tick they fall out, then leave the
        // mixer voice running silently (reverb tail decays naturally,
        // streaming source keeps consuming so it doesn't underrun if it
        // ever re-enters). No per-tick command spam.
        for (size_t i = processN; i < total; ++i) {
            EmitterRecord& rec = *interestSortScratch_[i].rec;
            if (rec.inInterestMute) continue;

            MixerCommand mute;
            mute.kind          = MixerCommandKind::UpdateParams;
            mute.mixSlot       = rec.assignedMixSlot;
            mute.gain          = 0.0f;
            mute.pan           = 0.0f;
            mute.pitch         = 1.0f;
            mute.lowPassAmount = 0.0f;
            mute.reverbSend    = 0.0f;
            mute.useBinaural   = false;
            mute.gainL         = 0.0f;
            mute.gainR         = 0.0f;
            mute.delaySamplesL = 0.0f;
            mute.delaySamplesR = 0.0f;
            mute.lpfAmountL    = 0.0f;
            mute.lpfAmountR    = 0.0f;
            mixer_->PostCommand(mute);

            rec.inInterestMute = true;
        }
    }

}

// Phase 10: drain the cross-thread voice packet ring into per-source
// jitter buffers, then have the codec decode and push PCM samples
// into each source's PCM ring. Skipped when voice is disabled.
void AudioRuntimeImpl::Update_Phase10_DrainVoicePackets_() noexcept {
    // -------------------------------------------------------------------
    // 10. Voice: drain the cross-thread voice packet ring into per-source
    //     jitter buffers, then have the codec decode and push PCM to each
    //     source's PCM ring.
    // -------------------------------------------------------------------
    if (config_.enableVoice && voiceCodec_) {
        VoicePacketCopy pkt;
        // Bounded drain.
        uint32_t budget = config_.voicePacketRingDepth * config_.budget.maxVoiceSources;
        while (budget-- > 0 && voicePackets_->Pop(pkt)) {
            voices_->OnPacket(pkt.playerId,
                                pkt.bytes.data(),
                                pkt.size,
                                pkt.sequenceNumber,
                                pkt.timestampMs,
                                pkt.arrivalMs);
        }
        voices_->DecodeAndPush(*voiceCodec_);
    }
}

// Phase 11: tick one-shot lifetime (free expired emitters + stop
// their mixer voices), publish the per-tick stats snapshot, then
// detect/log render-thread underrun deltas. Underrun log is
// delta-collapsed (one line per tick regardless of how many
// underruns happened) to avoid spamming on a flapping audio device.
void AudioRuntimeImpl::Update_Phase11_TickOneShotsAndPublishStats_(float deltaSeconds) noexcept {
    // -------------------------------------------------------------------
    // 11. Tick one-shot lifetime; free expired emitters and Stop their
    //     mixer voices.
    // -------------------------------------------------------------------
    {
        const double fps = static_cast<double>(config_.sampleRate);
        emitters_->TickOneShots(fps, deltaSeconds,
            [this](EmitterHandle, const EmitterRecord& rec) {
                StopMixerAndResetStreamingFor(rec);
            });
    }

    // Publish stats.
    statsLatest_.activeEmitters       = emitters_->Count();
    statsLatest_.activeVoiceSources   = voices_->Count();
    statsLatest_.mixerVoicesActive    = mixer_->ActiveVoicesApprox();
    statsLatest_.totalRenderCallbacks = mixer_->TotalCallbacks();
    statsLatest_.renderUnderruns      = mixer_->Underruns();

    // v0.18.0 Tier-A: bandwidth budget snapshot for the host network
    // thread. Published once per Update() tick; the host reads these
    // from the Stats accessor before deciding what to push. The
    // values are end-of-tick (after the rings have been drained by
    // Phases 2 and 4, before the next tick's submissions begin), so
    // they represent the maximum capacity the host can use without
    // hitting QueueFull on the next batch.
    //
    // The byte-budget heuristic: free slots × an average per-entry
    // size that approximates the wire cost of a typical event /
    // transform. We use 64 bytes per event (sizeof(AudioEvent) is
    // larger but most fields default-zero and would compress) and
    // 48 bytes per transform. Hosts that want the exact ring counts
    // should use the two `Remaining` fields directly; the byte
    // figure is for hosts wanting a single throttle signal.
    const size_t evtCap     = netEvents_->Capacity();
    const size_t evtUsed    = netEvents_->SizeApprox();
    const size_t evtFree    = (evtUsed < evtCap) ? (evtCap - evtUsed) : 0;
    const size_t xfCap      = netTransforms_->Capacity();
    const size_t xfUsed     = netTransforms_->SizeApprox();
    const size_t xfFree     = (xfUsed < xfCap) ? (xfCap - xfUsed) : 0;
    statsLatest_.eventRingCapacityRemaining =
        static_cast<uint32_t>(evtFree);
    statsLatest_.transformRingCapacityRemaining =
        static_cast<uint32_t>(xfFree);
    statsLatest_.nextTickProductionBudgetBytes =
        static_cast<uint32_t>(evtFree * 64u + xfFree * 48u);

    // Mixer-underrun delta detection. Underruns are incremented on the
    // render thread; we surface them as log events on the game thread
    // by diffing against last tick's snapshot. Each delta tick at
    // most one log line — bursts collapse into a single "underruns:
    // N" event so logs don't drown a flapping audio device.
    if (statsLatest_.renderUnderruns > lastUnderruns_) {
        const uint64_t delta = statsLatest_.renderUnderruns - lastUnderruns_;
        if (ShouldLog_(LogLevel::Warn)) {
            const LogField fields[] = {
                LogField::UInt("delta", delta),
                LogField::UInt("total", statsLatest_.renderUnderruns),
            };
            Log_(static_cast<uint8_t>(LogLevel::Warn),
                  LogCategory::kMixer,
                  "render-thread underrun(s) since last tick",
                  fields);
        }
        lastUnderruns_ = statsLatest_.renderUnderruns;
    }

}


AudioRuntime::Stats AudioRuntimeImpl::GetStats() const {
    AudioRuntime::Stats s = statsLatest_;
    if (mixer_) {
        s.mixerVoicesActive    = mixer_->ActiveVoicesApprox();
        s.totalRenderCallbacks = mixer_->TotalCallbacks();
        s.renderUnderruns      = mixer_->Underruns();
    }
    // Replication rate-limit aggregates. Cheap atomic loads.
    for (size_t i = 0; i < 6; ++i) {
        s.replicationEventsRateLimited[i] =
            replicationRateLimiter_.TotalRateLimitedForCategory(
                static_cast<AudioCategory>(i));
    }
    s.replicationEventsRejectedByValidator =
        replicationRateLimiter_.TotalValidatorRejections();
    s.replicationPolicyViolations =
        replicationRateLimiter_.TotalPolicyViolations();
    s.replicationEventsRejectedNewIdBudget =
        replicationRateLimiter_.TotalNewIdBudgetRejections();
    s.telemetrySinkExceptions =
        telemetrySinkExceptions_.load(std::memory_order_relaxed);
    s.logSinkExceptions =
        logSinkExceptions_.load(std::memory_order_relaxed);
    s.approxBytesAllocated = EstimateBaselineBytes(config_);

    // 2.4 + 2.6 voice counters: sum across all voice sources.
    if (voices_) {
        const auto agg = voices_->SnapshotCounters();
        s.voiceFramesDroppedDueToMute = agg.framesDroppedDueToMute;
        s.voiceBytesSent              = agg.bytesSent;
        s.voiceFramesBudgetDowngraded = agg.framesBudgetDowngraded;
        s.voiceFramesBudgetDropped    = agg.framesBudgetDropped;
    }
    return s;
}

void AudioRuntimeImpl::Log_(uint8_t                          level,
                              std::string_view                  category,
                              std::string_view                  message,
                              std::span<const LogField>         fields) const {
    // Caller must already have checked ShouldLog_(); this is the
    // mutex-and-emit half of the path. We re-check here defensively
    // so direct calls are safe even if someone bypasses the macro.
    if (logSink_ == nullptr) return;

    LogEvent e{
        controlClockMs_,
        static_cast<LogLevel>(level),
        category,
        message,
        fields,
    };
    // Mutex-serialize across game / network threads. Sinks therefore
    // don't need their own thread-safety. Brief lock — sink call
    // should be cheap. Defensive try/catch so a misbehaving host
    // sink can't break Update mid-flight; exception count surfaced
    // via Stats::logSinkExceptions so a buggy sink is observable.
    try {
        std::lock_guard<std::mutex> lk(logMutex_);
        logSink_->OnLogEvent(e);
    } catch (...) {
        // Counter is atomic because hook points fire from game and
        // network threads — read on game thread via GetStats.
        logSinkExceptions_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioRuntimeImpl::EmitTelemetry_(float deltaSeconds) {
    // Fast-path: no sink configured, or interval is zero (= disabled).
    // Branch-on-pointer is the cheapest possible no-op so the cost
    // stays under noise for projects that never wire telemetry.
    if (telemetrySink_ == nullptr || config_.telemetryIntervalMs == 0) {
        return;
    }

    // Accumulate elapsed wall time. We use deltaSeconds rather than
    // controlClockMs_ deltas because telemetryIntervalMs is meant to
    // be host-cadence-relative (the host calls Update at its own
    // rate); driving from controlClockMs_ would couple to network
    // tick latency and emit late under packet loss.
    telemetryAccumMs_ += static_cast<TimestampMs>(deltaSeconds * 1000.0f);
    if (telemetryAccumMs_ < config_.telemetryIntervalMs) {
        return;
    }
    // Reset the accumulator. We subtract rather than zero so a long
    // host frame doesn't lose telemetry samples — the next Update
    // catches up by emitting again immediately if still over budget.
    telemetryAccumMs_ -= config_.telemetryIntervalMs;

    // Build a fully-populated Stats snapshot the same way GetStats
    // does for callers. The sink sees exactly what GetStats would
    // return at this moment in time.
    AudioRuntime::Stats fresh = statsLatest_;
    if (mixer_) {
        fresh.mixerVoicesActive    = mixer_->ActiveVoicesApprox();
        fresh.totalRenderCallbacks = mixer_->TotalCallbacks();
        fresh.renderUnderruns      = mixer_->Underruns();
    }
    for (size_t i = 0; i < 6; ++i) {
        fresh.replicationEventsRateLimited[i] =
            replicationRateLimiter_.TotalRateLimitedForCategory(
                static_cast<AudioCategory>(i));
    }
    fresh.replicationEventsRejectedByValidator =
        replicationRateLimiter_.TotalValidatorRejections();
    fresh.replicationPolicyViolations =
        replicationRateLimiter_.TotalPolicyViolations();
    fresh.replicationEventsRejectedNewIdBudget =
        replicationRateLimiter_.TotalNewIdBudgetRejections();
    fresh.telemetrySinkExceptions =
        telemetrySinkExceptions_.load(std::memory_order_relaxed);
    fresh.logSinkExceptions =
        logSinkExceptions_.load(std::memory_order_relaxed);

    RuntimeStatsSample sample{controlClockMs_, fresh};
    // Sink may throw, but throwing across the runtime boundary
    // would leak Update mid-flight. Wrap defensively. Built-in
    // sinks never throw; misbehaving host sinks lose their sample
    // but the runtime keeps ticking — and the exception is now
    // surfaced via Stats::telemetrySinkExceptions for observability
    // (Stats can itself be reported through the same sink, so this
    // counter shows up on the next non-throwing sample).
    try {
        telemetrySink_->OnRuntimeStats(sample);
    } catch (...) {
        // Atomic only for symmetry with logSinkExceptions_; this
        // path is game-thread-only (EmitTelemetry_ is called from
        // Update step 12) so a plain increment would also be safe.
        telemetrySinkExceptions_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioRuntimeImpl::SetReplicationValidator(
        IReplicationValidator* validator) noexcept {
    // Pointer assignment to a single-word member is atomic enough on the
    // platforms we target; the validator runs only on the network thread,
    // so the game thread's setter is a coarse lifecycle operation, not a
    // hot path. Hosts that mutate this mid-flight should expect the
    // network thread to use either the old or the new validator on
    // events in flight.
    replicationValidator_ = validator;
}

bool AudioRuntimeImpl::GetPerPlayerReplicationStats(
        AudioPlayerId                              playerId,
        AudioRuntime::PerPlayerReplicationStats&   out) const {
    PerPlayerReplicationStats internal;
    if (!replicationRateLimiter_.GetPlayerStats(playerId, &internal)) {
        return false;
    }
    out.eventsAccepted    = internal.eventsAccepted;
    out.eventsRateLimited = internal.eventsRateLimited;
    out.eventsRejected    = internal.eventsRejected;
    return true;
}

// ---- v0.15.0: noexcept contract pins -----------------------------------
//
// These static_asserts make the noexcept guarantee a compile-time error
// to violate rather than a comment-only convention. If someone refactors
// Update() and accidentally drops the noexcept qualifier, the build
// breaks here with a clear message instead of the regression surfacing
// at runtime as an std::terminate in some host's game loop.
//
// Placed in the .cpp (not the header) so the assertion fires when this
// translation unit compiles — after the class is fully declared and
// the member function signature is known with its qualifiers.

static_assert(noexcept(std::declval<AudioRuntime&>().Update(0.0f)),
              "AudioRuntime::Update must be noexcept — the control-thread "
              "hot path is contractually exception-free. The body wraps a "
              "catch-all barrier; the qualifier makes it a compile-time "
              "guarantee. See v0.15.0 hardening notes in CHANGELOG.md.");

static_assert(noexcept(std::declval<AudioRuntimeImpl&>().Update(0.0f)),
              "AudioRuntimeImpl::Update must be noexcept (the impl forwarder "
              "matches the public signature; this pin catches drift between "
              "the two layers).");

} // namespace audio
