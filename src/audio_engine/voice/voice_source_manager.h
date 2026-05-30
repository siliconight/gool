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

// audio_engine/voice/voice_source_manager.h
//
// Owns one VoiceSource per registered remote player. Each source has its
// own jitter buffer and decoded PCM ring. Network-thread Push (raw bytes) ->
// control-thread Decode-on-tick (drain jitter buffer, codec decode, push
// PCM to ring) -> render-thread Pull from ring during OnRender.
//
// Render thread holds a stable PcmRing pointer (handed in via the mixer's
// StartVoice command). Control thread mutates the ring as producer.

#ifndef AUDIO_ENGINE_VOICE_VOICE_SOURCE_MANAGER_H
#define AUDIO_ENGINE_VOICE_VOICE_SOURCE_MANAGER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "audio_engine/handles.h"
#include "audio_engine/result.h"
#include "audio_engine/types.h"
#include "audio_engine/voice_codec.h"
#include "audio_engine/util/pcm_ring.h"
#include "audio_engine/util/slot_map.h"
#include "audio_engine/voice/jitter_buffer.h"

namespace audio {

struct VoiceSourceRecord {
    AudioPlayerId playerId    = kInvalidPlayerId;
    uint32_t      mixSlot     = 0;        // assigned at registration
    bool          mixerStarted = false;

    std::unique_ptr<voice::JitterBuffer>  jitter;
    std::unique_ptr<util::PcmRing>        pcmRing;

    // --- 2.4 mute/volume state (set: game thread; read: control thread).
    // std::atomic<float> support was clarified in C++20; we're on C++20.
    // Use atomic<bool>/atomic<float> rather than a mutex so the
    // control-thread DecodeAndPush hot path is lock-free.
    std::atomic<bool>  muted{false};
    std::atomic<float> volume{1.0f};

    // Frames that would have been decoded but were dropped because the
    // source was muted. Single-writer (control thread in DecodeAndPush),
    // single-reader (game thread via GetStats). uint64 increments are
    // not torn on any platform we support — same pattern as
    // JitterBufferStats.
    uint64_t framesDroppedDueToMute = 0;

    // --- 2.6 bandwidth-budget state.
    // Set: game thread via SetVoiceBandwidthBudget(playerId, bytesPerSec).
    // Read: any thread calling Suggest/Report.
    // 0 = no budget enforced (default; SuggestVoiceBitrate returns
    // the default 32000 bps regardless of bucket).
    std::atomic<uint32_t> bandwidthBudgetBytesPerSec{0};

    // Token bucket. Tokens are in BYTES (not packets / frames), since
    // the budget is bytes/sec. Capacity == 1 second of budget (allows
    // a small burst over the steady-state rate). Refilled in Suggest()
    // proportional to elapsed wall-clock since lastRefillNs.
    //
    // Accessed only from one host thread per source in normal usage;
    // we still use atomics to defend against concurrent Suggest/Report
    // from threads the host might split across.
    std::atomic<int64_t> bucketTokens{0};      // signed: can go briefly negative on burst
    std::atomic<uint64_t> bucketLastRefillNs{0};

    // Per-source byte-sent accumulator (host calls ReportVoiceBytesSent).
    // Surfaced via Stats::voiceBytesSent (summed across all sources).
    uint64_t bytesSent              = 0;
    uint64_t framesBudgetDowngraded = 0;
    uint64_t framesBudgetDropped    = 0;

    // Default-construct, no copy, custom move. std::atomic<T> has
    // deleted copy/move ops; SlotMap needs MoveInsertable+MoveAssignable,
    // so we provide our own that .load()/.store() the atomic fields.
    // The move only happens at registration time, before any concurrent
    // reader exists, so relaxed ordering is fine here.
    VoiceSourceRecord() = default;

    VoiceSourceRecord(const VoiceSourceRecord&)            = delete;
    VoiceSourceRecord& operator=(const VoiceSourceRecord&) = delete;

    VoiceSourceRecord(VoiceSourceRecord&& o) noexcept
        : playerId(o.playerId),
          mixSlot(o.mixSlot),
          mixerStarted(o.mixerStarted),
          jitter(std::move(o.jitter)),
          pcmRing(std::move(o.pcmRing)),
          muted(o.muted.load(std::memory_order_relaxed)),
          volume(o.volume.load(std::memory_order_relaxed)),
          framesDroppedDueToMute(o.framesDroppedDueToMute),
          bandwidthBudgetBytesPerSec(o.bandwidthBudgetBytesPerSec.load(std::memory_order_relaxed)),
          bucketTokens(o.bucketTokens.load(std::memory_order_relaxed)),
          bucketLastRefillNs(o.bucketLastRefillNs.load(std::memory_order_relaxed)),
          bytesSent(o.bytesSent),
          framesBudgetDowngraded(o.framesBudgetDowngraded),
          framesBudgetDropped(o.framesBudgetDropped) {}

    VoiceSourceRecord& operator=(VoiceSourceRecord&& o) noexcept {
        if (this == &o) return *this;
        playerId               = o.playerId;
        mixSlot                = o.mixSlot;
        mixerStarted           = o.mixerStarted;
        jitter                 = std::move(o.jitter);
        pcmRing                = std::move(o.pcmRing);
        muted.store(o.muted.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
        volume.store(o.volume.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
        framesDroppedDueToMute = o.framesDroppedDueToMute;
        bandwidthBudgetBytesPerSec.store(
            o.bandwidthBudgetBytesPerSec.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        bucketTokens.store(o.bucketTokens.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
        bucketLastRefillNs.store(o.bucketLastRefillNs.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
        bytesSent              = o.bytesSent;
        framesBudgetDowngraded = o.framesBudgetDowngraded;
        framesBudgetDropped    = o.framesBudgetDropped;
        return *this;
    }
};

class VoiceSourceManager {
public:
    VoiceSourceManager(uint32_t maxVoiceSources,
                        uint32_t pcmRingFrames,
                        uint32_t packetRingDepth,
                        uint32_t maxPacketBytes,
                        uint32_t mixSlotBase);

    // Game thread
    Result<VoiceSourceHandle> Register(AudioPlayerId playerId);
    AudioResult               Unregister(VoiceSourceHandle h);

    // Game thread. Both setters return Success / NotFound /
    // InvalidArgument. InvalidHandle: no voice source registered for that
    // player. InvalidArgument: volume out of [0, 4] sanity range.
    // Volume clamp is generous (>1 allows boost above unity).
    AudioResult SetMuted(AudioPlayerId playerId, bool muted);
    AudioResult SetVolume(AudioPlayerId playerId, float volume);

    // Game thread. Returns false for unknown players; out remains
    // default-constructed in that case.
    bool  GetMuted(AudioPlayerId playerId, bool& out)   const noexcept;
    bool  GetVolume(AudioPlayerId playerId, float& out) const noexcept;

    // Game thread. Set the upstream-bytes/sec budget for this source.
    // 0 = no enforcement (default). Returns NotFound if the player isn't
    // registered.
    AudioResult SetBandwidthBudget(AudioPlayerId playerId, uint32_t bytesPerSec);

    // Any host-managed thread. Consult the budget for a voice frame.
    // Returns the suggested Opus bitrate in bps (one of 32000 / 24000
    // / 16000), or 0 if the host should drop this frame entirely.
    // Refills the token bucket based on elapsed wall time.
    //
    // The bytes/frame estimate at each rung uses approximate
    // packet-size models for the given frame duration:
    //   32 kbps: ~80 B/frame at 20ms, ~40 at 10ms
    //   24 kbps: ~60 B/frame at 20ms, ~30 at 10ms
    //   16 kbps: ~40 B/frame at 20ms, ~20 at 10ms
    // (Plus a small UDP/RTP overhead fudge.)
    //
    // Returns 32000 if no budget is set for this source.
    int32_t SuggestBitrate(AudioPlayerId playerId, uint32_t frameDurationMs) noexcept;

    // Host reports the actual encoded packet size after sending. Spent
    // bytes are deducted from the bucket. Also increments per-source
    // bytesSent and (if the host honored a downgraded suggestion) the
    // framesBudgetDowngraded counter.
    AudioResult ReportBytesSent(AudioPlayerId playerId, uint32_t bytes,
                                  int32_t bitrateUsedBps) noexcept;

    // Network thread. Returns InvalidHandle if no source for that player.
    AudioResult OnPacket(AudioPlayerId  playerId,
                          const uint8_t* bytes,
                          size_t         size,
                          uint16_t       sequenceNumber,
                          TimestampMs    sendTimestampMs,
                          TimestampMs    arrivalTimestampMs);

    // Control thread. Drains all jitter buffers and decodes with the given
    // codec. Returns the number of frames decoded across all sources this
    // tick.
    uint32_t DecodeAndPush(IVoiceCodec& codec);

    // Game thread. Read-only snapshot of per-player jitter-buffer
    // telemetry: packets received/late/lost/PLC, current jitter ms,
    // current target depth. Returns nullptr if the player isn't
    // registered.
    const voice::JitterBufferStats* GetVoiceStats(AudioPlayerId playerId) const noexcept;

    // Game thread. Bumps the per-player packetsRateLimited counter
    // when the upstream rate limiter rejected a voice packet. No-op
    // if the player has no registered voice source (the global
    // Stats::replicationEventsRateLimited[Voice] still records the
    // rejection).
    void BumpVoicePacketRateLimited(AudioPlayerId playerId) noexcept;

    // Sum-across-sources accessor for AudioRuntime::GetStats(). Caller is
    // game thread; reads are torn-free for uint64 monotone increments
    // on the platforms we support (same model as JitterBufferStats).
    struct AggregateCounters {
        uint64_t framesDroppedDueToMute = 0;
        uint64_t bytesSent              = 0;
        uint64_t framesBudgetDowngraded = 0;
        uint64_t framesBudgetDropped    = 0;
    };
    AggregateCounters SnapshotCounters() const noexcept;

    // Iterate active records (for the runtime to send mixer StartVoice
    // commands when registering, and Stop when unregistering).
    template <typename F>
    void ForEach(F&& fn) {
        sources_.ForEach(std::forward<F>(fn));
    }

    uint32_t Count()        const noexcept { return sources_.Count(); }
    uint32_t Capacity()     const noexcept { return sources_.Capacity(); }
    uint32_t MixSlotBase()  const noexcept { return mixSlotBase_; }

    VoiceSourceRecord*       Get(VoiceSourceHandle h)       noexcept { return sources_.Get(h); }
    const VoiceSourceRecord* Get(VoiceSourceHandle h) const noexcept { return sources_.Get(h); }

private:
    // Refill helper. Returns the bucket's current token count after
    // refilling from elapsed time. Called by Suggest before consulting
    // the bucket, and by Report after spending.
    int64_t RefillBucket(VoiceSourceRecord& rec) noexcept;

    util::SlotMap<VoiceSourceHandle, VoiceSourceRecord> sources_;
    std::unordered_map<AudioPlayerId, VoiceSourceHandle> byPlayer_;

    uint32_t pcmRingFrames_;
    uint32_t packetRingDepth_;
    uint32_t maxPacketBytes_;
    uint32_t mixSlotBase_;
};

} // namespace audio

#endif // AUDIO_ENGINE_VOICE_VOICE_SOURCE_MANAGER_H
