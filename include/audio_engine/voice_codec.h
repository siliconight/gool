// audio_engine/voice_codec.h
//
// Voice codec seam. Encode and decode boundary; one of the four real
// polymorphism seams. Two implementations ship: StubVoiceCodec
// (pass-through int16 PCM, useful for local testing and as a fallback)
// and OpusVoiceCodec (libopus, opt-in via AUDIO_ENGINE_VOICE_OPUS).

#ifndef AUDIO_ENGINE_VOICE_CODEC_H
#define AUDIO_ENGINE_VOICE_CODEC_H

#include <cstdint>
#include <cstddef>
#include "audio_engine/types.h"
#include "audio_engine/result.h"

namespace audio {

struct VoicePacket {
    AudioPlayerId  playerId       = kInvalidPlayerId;
    uint16_t       sequenceNumber = 0;
    TimestampMs    timestampMs    = 0;
    const uint8_t* data           = nullptr;    // borrowed; copied on receive
    size_t         size           = 0;
};

class IVoiceCodec {
public:
    virtual ~IVoiceCodec() = default;

    virtual const char* Name() const noexcept = 0;

    // Frame size and channels are fixed for the lifetime of the codec
    // instance; callers size buffers accordingly.
    virtual uint32_t FrameSize()  const noexcept = 0;   // samples per frame, per channel
    virtual uint32_t SampleRate() const noexcept = 0;
    virtual uint32_t Channels()   const noexcept = 0;

    // Decode one packet into a caller-supplied output buffer. Buffer size
    // must be at least FrameSize() * Channels() int16_t samples.
    // outFrames is set to the number of frames written.
    // Implementations must not allocate.
    virtual AudioResult Decode(
        const VoicePacket& packet,
        int16_t*           output,
        uint32_t           outputCapacityFrames,
        uint32_t&          outFrames) noexcept = 0;

    // Generate concealment audio for a lost packet (PLC — packet loss
    // concealment). Called by the jitter buffer when a packet at the
    // expected sequence position is missing. The codec produces one
    // frame of plausible signal extrapolated from prior decoded
    // context: Opus has built-in PLC; the stub returns silence.
    //
    // The host must call this for the SAME player whose stream had
    // the gap, so per-player decoder state advances coherently. A
    // PLC call costs the codec one frame of "decoded" history so the
    // next real packet decoded for that player crossfades cleanly.
    //
    // Returns Success on PLC produced; Unsupported if the codec does
    // not implement PLC (caller falls back to silence). Default
    // implementation returns Unsupported.
    virtual AudioResult DecodeLost(
        AudioPlayerId /*playerId*/,
        int16_t*      /*output*/,
        uint32_t      /*outputCapacityFrames*/,
        uint32_t&     outFrames) noexcept {
        outFrames = 0;
        return AudioResult::Unsupported;
    }

    // Encode one frame's worth of PCM into outBytes. outSize is set to the
    // number of bytes written. Returns BudgetExceeded if outCapacity is too
    // small. Implementations must not allocate.
    virtual AudioResult Encode(
        const int16_t* input,
        uint32_t       frameCount,
        uint8_t*       outBytes,
        size_t         outCapacity,
        size_t&        outSize) noexcept = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_VOICE_CODEC_H
