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

// audio_engine/voice/stub_voice_codec.h
//
// Pass-through "codec". Encode just memcpys input PCM to output bytes;
// Decode does the reverse. Useful for local testing and as a default when
// Opus isn't wired in. Configured for 20 ms frames at 48 kHz mono (the
// shape Opus would normally produce).

#ifndef AUDIO_ENGINE_VOICE_STUB_VOICE_CODEC_H
#define AUDIO_ENGINE_VOICE_STUB_VOICE_CODEC_H

#include "audio_engine/voice_codec.h"

namespace audio {

class StubVoiceCodec final : public IVoiceCodec {
public:
    explicit StubVoiceCodec(uint32_t sampleRate = 48000,
                   uint32_t channels   = 1,
                   uint32_t frameSize  = 960);   // 20 ms at 48 kHz

    const char* Name()       const noexcept override { return "stub-pcm"; }
    uint32_t    FrameSize()  const noexcept override { return frameSize_; }
    uint32_t    SampleRate() const noexcept override { return sampleRate_; }
    uint32_t    Channels()   const noexcept override { return channels_; }

    AudioResult Decode(const VoicePacket& packet,
                        int16_t*           output,
                        uint32_t           outputCapacityFrames,
                        uint32_t&          outFrames) noexcept override;

    AudioResult Encode(const int16_t* input,
                        uint32_t       frameCount,
                        uint8_t*       outBytes,
                        size_t         outCapacity,
                        size_t&        outSize) noexcept override;

private:
    uint32_t sampleRate_;
    uint32_t channels_;
    uint32_t frameSize_;
};

} // namespace audio

#endif // AUDIO_ENGINE_VOICE_STUB_VOICE_CODEC_H
