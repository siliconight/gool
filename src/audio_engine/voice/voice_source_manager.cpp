// audio_engine/voice/voice_source_manager.cpp

#include "audio_engine/voice/voice_source_manager.h"

#include <algorithm>
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
                rec.pcmRing->Push(pcmScratch.data(), decoded);
                totalFrames += decoded;
            }
        }
    });

    return totalFrames;
}

} // namespace audio
