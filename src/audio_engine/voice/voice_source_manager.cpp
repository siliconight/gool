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

// audio_engine/voice/voice_source_manager.cpp

#include "audio_engine/voice/voice_source_manager.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace audio {

VoiceSourceManager::VoiceSourceManager(uint32_t maxVoiceSources,
                                         uint32_t pcmRingFrames,
                                         uint32_t packetRingDepth,
                                         uint32_t maxPacketBytes,
                                         uint32_t mixSlotBase)
    : sources_(maxVoiceSources),
      pcmRingFrames_(pcmRingFrames),
      packetRingDepth_(packetRingDepth),
      maxPacketBytes_(maxPacketBytes),
      mixSlotBase_(mixSlotBase) {
    byPlayer_.reserve(maxVoiceSources);
}

Result<VoiceSourceHandle> VoiceSourceManager::Register(AudioPlayerId playerId) {
    if (playerId == kInvalidPlayerId) return AudioResult::InvalidArgument;

    auto it = byPlayer_.find(playerId);
    if (it != byPlayer_.end()) {
        return it->second;        // idempotent: return existing
    }

    VoiceSourceRecord rec;
    rec.playerId = playerId;
    // Channels=1: voice is mono. The codec dictates the sample rate.
    rec.pcmRing = std::make_unique<util::PcmRing>(pcmRingFrames_, 1u);
    rec.jitter  = std::make_unique<voice::JitterBuffer>(packetRingDepth_, maxPacketBytes_);

    auto handle = sources_.Allocate(std::move(rec));
    if (!handle) return AudioResult::BudgetExceeded;

    auto* allocated = sources_.Get(*handle);
    allocated->mixSlot = mixSlotBase_ + (handle->index - 1);
    byPlayer_[playerId] = *handle;
    return *handle;
}

AudioResult VoiceSourceManager::Unregister(VoiceSourceHandle h) {
    auto* rec = sources_.Get(h);
    if (!rec) return AudioResult::InvalidHandle;
    byPlayer_.erase(rec->playerId);
    if (!sources_.Free(h)) return AudioResult::InvalidHandle;
    return AudioResult::Success;
}

AudioResult VoiceSourceManager::OnPacket(AudioPlayerId  playerId,
                                           const uint8_t* bytes,
                                           size_t         size,
                                           uint16_t       sequenceNumber,
                                           TimestampMs    sendTimestampMs,
                                           TimestampMs    arrivalTimestampMs) {
    auto it = byPlayer_.find(playerId);
    if (it == byPlayer_.end()) return AudioResult::InvalidHandle;

    auto* rec = sources_.Get(it->second);
    if (!rec || !rec->jitter) return AudioResult::InvalidHandle;

    if (!rec->jitter->Push(sequenceNumber, sendTimestampMs, arrivalTimestampMs,
                             bytes, size)) {
        return AudioResult::BudgetExceeded;
    }
    return AudioResult::Success;
}

const voice::JitterBufferStats*
VoiceSourceManager::GetVoiceStats(AudioPlayerId playerId) const noexcept {
    auto it = byPlayer_.find(playerId);
    if (it == byPlayer_.end()) return nullptr;
    auto* rec = sources_.Get(it->second);
    if (!rec || !rec->jitter) return nullptr;
    return &rec->jitter->Stats();
}

void VoiceSourceManager::BumpVoicePacketRateLimited(
        AudioPlayerId playerId) noexcept {
    auto it = byPlayer_.find(playerId);
    if (it == byPlayer_.end()) return;
    auto* rec = sources_.Get(it->second);
    if (!rec || !rec->jitter) return;
    rec->jitter->BumpRateLimited();
}

uint32_t VoiceSourceManager::DecodeAndPush(IVoiceCodec& codec) {
    const uint32_t frameSize = codec.FrameSize();
    const uint32_t channels  = codec.Channels();
    if (frameSize == 0 || channels == 0) return 0;

    // Scratch sized for one decode frame.
    thread_local std::vector<int16_t> pcmScratch;
    if (pcmScratch.size() < static_cast<size_t>(frameSize) * channels) {
        pcmScratch.resize(static_cast<size_t>(frameSize) * channels);
    }
    thread_local std::vector<uint8_t> packetScratch;
    if (packetScratch.size() < maxPacketBytes_) {
        packetScratch.resize(maxPacketBytes_);
    }

    uint32_t totalFrames = 0;
    sources_.ForEach([&](VoiceSourceHandle, VoiceSourceRecord& rec) {
        if (!rec.jitter || !rec.pcmRing) return;

        // --- 2.4 mute: drain jitter buffer, skip codec.Decode, count drops.
        // We DO drain so the jitter buffer doesn't accumulate stale
        // packets for the whole duration of the mute. We DON'T decode
        // (saves Opus CPU — the explicit DoD outcome) and DON'T push
        // anything to the PCM ring (the mixer naturally produces
        // silence as the ring drains).
        if (rec.muted.load(std::memory_order_acquire)) {
            const uint32_t maxIter = rec.jitter->Depth();
            for (uint32_t i = 0; i < maxIter; ++i) {
                size_t      drainedSize = 0;
                TimestampMs drainedTs   = 0;
                const auto  r = rec.jitter->PopNext(packetScratch.data(),
                                                      packetScratch.size(),
                                                      drainedSize,
                                                      drainedTs);
                if (r == voice::PopResult::Empty) break;
                if (r == voice::PopResult::Packet) {
                    // Count one frame as dropped per real packet dropped.
                    // PLC doesn't count (it's a synthesized frame, not a
                    // dropped real one).
                    rec.framesDroppedDueToMute += frameSize;
                }
            }
            return;
        }

        // Cache volume once per source per tick to avoid repeated atomic
        // loads. Volume changes between ticks are rare; readers don't
        // need strong ordering.
        const float volume = rec.volume.load(std::memory_order_relaxed);

        // Drain at most `depth` outcomes per source per tick. Each
        // outcome is one of: a real packet decoded, a PLC frame
        // generated, or a stop signal (Empty). The Empty case ends
        // this source's drain — there's no more to do until more
        // packets arrive.
        const uint32_t maxIter = rec.jitter->Depth();
        for (uint32_t i = 0; i < maxIter; ++i) {
            size_t      gotSize = 0;
            TimestampMs gotTs   = 0;
            const auto  result  = rec.jitter->PopNext(packetScratch.data(),
                                                        packetScratch.size(),
                                                        gotSize,
                                                        gotTs);
            if (result == voice::PopResult::Empty) break;

            uint32_t decoded = 0;

            if (result == voice::PopResult::Packet) {
                if (gotSize == 0) continue;
                VoicePacket pkt;
                pkt.playerId    = rec.playerId;
                pkt.timestampMs = gotTs;
                pkt.data        = packetScratch.data();
                pkt.size        = gotSize;
                if (codec.Decode(pkt, pcmScratch.data(), frameSize, decoded)
                    != AudioResult::Success) {
                    decoded = 0;
                }
            } else {
                // PLC. Ask the codec to extrapolate one frame; if the
                // codec doesn't implement PLC, fall back to silence
                // (which still keeps the decoder's per-player state
                // advancing correctly because we wrote zeros into
                // the pcm ring for one frame).
                const auto rc = codec.DecodeLost(rec.playerId,
                                                   pcmScratch.data(),
                                                   frameSize,
                                                   decoded);
                if (rc != AudioResult::Success || decoded == 0) {
                    // Silence one frame.
                    std::fill(pcmScratch.begin(),
                              pcmScratch.begin() + static_cast<ptrdiff_t>(frameSize) * channels,
                              static_cast<int16_t>(0));
                    decoded = frameSize;
                }
            }

            if (decoded > 0) {
                // --- 2.4 volume: scale int16 PCM by per-source volume
                // before pushing to the ring. Applied here (control
                // thread) rather than in the mixer (render thread) to
                // keep the mixer's MixVoice gain logic untouched and
                // because volume changes are low-frequency enough that
                // an extra multiply per decoded sample is cheap.
                // Quantization noise floor for int16 PCM at volume=0.5
                // is ~-90 dBFS — well below speech noise floor.
                if (volume != 1.0f) {
                    const size_t total = static_cast<size_t>(decoded) * channels;
                    for (size_t s = 0; s < total; ++s) {
                        const float scaled = static_cast<float>(pcmScratch[s]) * volume;
                        // Clamp to int16 range.
                        const float clamped =
                            (scaled >  32767.0f) ?  32767.0f :
                            (scaled < -32768.0f) ? -32768.0f : scaled;
                        pcmScratch[s] = static_cast<int16_t>(clamped);
                    }
                }
                rec.pcmRing->Push(pcmScratch.data(), decoded);
                totalFrames += decoded;
            }
        }
    });

    return totalFrames;
}

// ---------------------------------------------------------------------------
// 2.4 mute/volume
// ---------------------------------------------------------------------------

AudioResult VoiceSourceManager::SetMuted(AudioPlayerId playerId, bool muted) {
    auto it = byPlayer_.find(playerId);
    if (it == byPlayer_.end()) return AudioResult::InvalidHandle;
    auto* rec = sources_.Get(it->second);
    if (!rec) return AudioResult::InvalidHandle;
    rec->muted.store(muted, std::memory_order_release);
    return AudioResult::Success;
}

AudioResult VoiceSourceManager::SetVolume(AudioPlayerId playerId, float volume) {
    if (!(volume >= 0.0f) || volume > 4.0f) return AudioResult::InvalidArgument;
    auto it = byPlayer_.find(playerId);
    if (it == byPlayer_.end()) return AudioResult::InvalidHandle;
    auto* rec = sources_.Get(it->second);
    if (!rec) return AudioResult::InvalidHandle;
    rec->volume.store(volume, std::memory_order_release);
    return AudioResult::Success;
}

bool VoiceSourceManager::GetMuted(AudioPlayerId playerId, bool& out) const noexcept {
    auto it = byPlayer_.find(playerId);
    if (it == byPlayer_.end()) return false;
    const auto* rec = sources_.Get(it->second);
    if (!rec) return false;
    out = rec->muted.load(std::memory_order_acquire);
    return true;
}

bool VoiceSourceManager::GetVolume(AudioPlayerId playerId, float& out) const noexcept {
    auto it = byPlayer_.find(playerId);
    if (it == byPlayer_.end()) return false;
    const auto* rec = sources_.Get(it->second);
    if (!rec) return false;
    out = rec->volume.load(std::memory_order_acquire);
    return true;
}

// ---------------------------------------------------------------------------
// 2.6 bandwidth-budget hooks
// ---------------------------------------------------------------------------

namespace {

// Steady_clock-derived "now in nanoseconds" — single source for the
// bucket refill timestamp. We don't store the std::chrono type
// directly because token-bucket atomics need a POD integral.
uint64_t NowNs() noexcept {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
}

// Estimate bytes per packet at a given bitrate and frame duration.
// Returns the bandwidth-side cost; includes a small overhead fudge
// (~12 bytes) for RTP/UDP headers so the budget doesn't over-promise.
constexpr uint32_t kHeaderOverheadBytes = 12;
uint32_t EstimatePacketBytes(int32_t bitrateBps, uint32_t frameDurationMs) noexcept {
    if (bitrateBps <= 0 || frameDurationMs == 0) return 0;
    // bits-per-frame = bitrate * frameMs / 1000  →  bytes = bits / 8
    const uint64_t bitsPerFrame =
        static_cast<uint64_t>(bitrateBps) * frameDurationMs / 1000ull;
    return static_cast<uint32_t>(bitsPerFrame / 8ull) + kHeaderOverheadBytes;
}

// Bitrate ladder. Listed high-to-low so SuggestBitrate can pick the
// highest rung whose cost fits the bucket.
constexpr int32_t kBitrateLadder[] = { 32000, 24000, 16000 };
constexpr int32_t kDefaultBitrate  = 32000;

} // namespace

int64_t VoiceSourceManager::RefillBucket(VoiceSourceRecord& rec) noexcept {
    const uint32_t budget = rec.bandwidthBudgetBytesPerSec.load(std::memory_order_acquire);
    if (budget == 0) return 0;

    const uint64_t now = NowNs();
    uint64_t last = rec.bucketLastRefillNs.load(std::memory_order_acquire);
    if (last == 0) {
        // First call. Initialize bucket to one second's worth of budget.
        rec.bucketLastRefillNs.store(now, std::memory_order_release);
        rec.bucketTokens.store(static_cast<int64_t>(budget),
                                std::memory_order_release);
        return static_cast<int64_t>(budget);
    }

    if (now <= last) {
        return rec.bucketTokens.load(std::memory_order_acquire);
    }
    const uint64_t elapsedNs = now - last;

    // tokens_added = elapsed_seconds * budget = elapsedNs * budget / 1e9
    const int64_t tokensAdded =
        static_cast<int64_t>((elapsedNs * static_cast<uint64_t>(budget)) / 1'000'000'000ull);

    if (tokensAdded == 0) {
        // Not enough time elapsed for a single-byte refill — leave state
        // alone, return current. Avoids losing fractional tokens by
        // updating timestamps without crediting them.
        return rec.bucketTokens.load(std::memory_order_acquire);
    }

    int64_t tokens = rec.bucketTokens.load(std::memory_order_acquire) + tokensAdded;
    // Cap at one second of budget (the "burst capacity").
    const int64_t cap = static_cast<int64_t>(budget);
    if (tokens > cap) tokens = cap;

    rec.bucketTokens.store(tokens, std::memory_order_release);
    rec.bucketLastRefillNs.store(now, std::memory_order_release);
    return tokens;
}

AudioResult VoiceSourceManager::SetBandwidthBudget(
        AudioPlayerId playerId, uint32_t bytesPerSec) {
    auto it = byPlayer_.find(playerId);
    if (it == byPlayer_.end()) return AudioResult::InvalidHandle;
    auto* rec = sources_.Get(it->second);
    if (!rec) return AudioResult::InvalidHandle;
    rec->bandwidthBudgetBytesPerSec.store(bytesPerSec, std::memory_order_release);
    // Reset bucket state — a budget change starts fresh.
    rec->bucketLastRefillNs.store(0, std::memory_order_release);
    rec->bucketTokens.store(0, std::memory_order_release);
    return AudioResult::Success;
}

int32_t VoiceSourceManager::SuggestBitrate(
        AudioPlayerId playerId, uint32_t frameDurationMs) noexcept {
    auto it = byPlayer_.find(playerId);
    if (it == byPlayer_.end()) return kDefaultBitrate;
    auto* rec = sources_.Get(it->second);
    if (!rec) return kDefaultBitrate;

    const uint32_t budget = rec->bandwidthBudgetBytesPerSec.load(std::memory_order_acquire);
    if (budget == 0) return kDefaultBitrate;     // no enforcement
    if (frameDurationMs == 0) return kDefaultBitrate;

    const int64_t tokens = RefillBucket(*rec);

    // Pick highest rung whose cost fits the bucket.
    for (const int32_t rate : kBitrateLadder) {
        const uint32_t cost = EstimatePacketBytes(rate, frameDurationMs);
        if (static_cast<int64_t>(cost) <= tokens) {
            return rate;
        }
    }
    // No rung fits — host should drop this frame. Increment drop counter
    // here at the policy decision boundary so the count is accurate
    // even if the host forgets to call a separate "I dropped it" API.
    rec->framesBudgetDropped++;
    return 0;     // drop
}

AudioResult VoiceSourceManager::ReportBytesSent(
        AudioPlayerId playerId, uint32_t bytes, int32_t bitrateUsedBps) noexcept {
    auto it = byPlayer_.find(playerId);
    if (it == byPlayer_.end()) return AudioResult::InvalidHandle;
    auto* rec = sources_.Get(it->second);
    if (!rec) return AudioResult::InvalidHandle;

    rec->bytesSent += bytes;

    // Was the bitrate downgraded vs the default? If so, bump the
    // downgrade counter. (Drops are counted at the SuggestBitrate==0
    // boundary — the host is expected to NOT call ReportBytesSent for
    // a dropped frame.)
    if (bitrateUsedBps > 0 && bitrateUsedBps < kDefaultBitrate) {
        rec->framesBudgetDowngraded++;
    }

    // Deduct from bucket (only meaningful if a budget is set).
    const uint32_t budget = rec->bandwidthBudgetBytesPerSec.load(std::memory_order_acquire);
    if (budget > 0) {
        int64_t tokens = rec->bucketTokens.load(std::memory_order_acquire);
        tokens -= static_cast<int64_t>(bytes);
        rec->bucketTokens.store(tokens, std::memory_order_release);
    }
    return AudioResult::Success;
}

VoiceSourceManager::AggregateCounters
VoiceSourceManager::SnapshotCounters() const noexcept {
    AggregateCounters agg;
    sources_.ForEach([&](VoiceSourceHandle, const VoiceSourceRecord& rec) {
        agg.framesDroppedDueToMute += rec.framesDroppedDueToMute;
        agg.bytesSent              += rec.bytesSent;
        agg.framesBudgetDowngraded += rec.framesBudgetDowngraded;
        agg.framesBudgetDropped    += rec.framesBudgetDropped;
    });
    return agg;
}

} // namespace audio
