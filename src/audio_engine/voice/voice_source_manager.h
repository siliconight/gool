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
    util::SlotMap<VoiceSourceHandle, VoiceSourceRecord> sources_;
    std::unordered_map<AudioPlayerId, VoiceSourceHandle> byPlayer_;

    uint32_t pcmRingFrames_;
    uint32_t packetRingDepth_;
    uint32_t maxPacketBytes_;
    uint32_t mixSlotBase_;
};

} // namespace audio

#endif // AUDIO_ENGINE_VOICE_VOICE_SOURCE_MANAGER_H
