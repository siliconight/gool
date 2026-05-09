// audio_engine/voice/stub_voice_codec.cpp

#include "audio_engine/voice/stub_voice_codec.h"

#include <cstring>

namespace audio {

StubVoiceCodec::StubVoiceCodec(uint32_t sampleRate,
                                 uint32_t channels,
                                 uint32_t frameSize)
    : sampleRate_(sampleRate),
      channels_(channels),
      frameSize_(frameSize) {}

AudioResult StubVoiceCodec::Decode(const VoicePacket& packet,
                                     int16_t*           output,
                                     uint32_t           outputCapacityFrames,
                                     uint32_t&          outFrames) noexcept {
    outFrames = 0;
    if (!output || !packet.data || packet.size == 0) {
        return AudioResult::InvalidArgument;
    }

    const size_t bytesPerFrame = sizeof(int16_t) * channels_;
    if (bytesPerFrame == 0) return AudioResult::InternalError;

    uint32_t framesInPacket = static_cast<uint32_t>(packet.size / bytesPerFrame);
    if (framesInPacket == 0) return AudioResult::InvalidArgument;

    if (framesInPacket > outputCapacityFrames) {
        framesInPacket = outputCapacityFrames;
    }

    std::memcpy(output, packet.data, framesInPacket * bytesPerFrame);
    outFrames = framesInPacket;
    return AudioResult::Success;
}

AudioResult StubVoiceCodec::Encode(const int16_t* input,
                                     uint32_t       frameCount,
                                     uint8_t*       outBytes,
                                     size_t         outCapacity,
                                     size_t&        outSize) noexcept {
    outSize = 0;
    if (!input || !outBytes) return AudioResult::InvalidArgument;

    const size_t needed = static_cast<size_t>(frameCount) * channels_ * sizeof(int16_t);
    if (needed > outCapacity) return AudioResult::BudgetExceeded;

    std::memcpy(outBytes, input, needed);
    outSize = needed;
    return AudioResult::Success;
}

} // namespace audio
