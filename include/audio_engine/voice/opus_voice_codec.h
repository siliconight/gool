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

// audio_engine/voice/opus_voice_codec.h
//
// IVoiceCodec implementation backed by libopus. Conditionally compiled;
// when AUDIO_ENGINE_VOICE_OPUS is undefined, the constructor reports the
// codec as unsupported and Encode/Decode return AudioResult::Unsupported,
// so host code can reference the type without a preprocessor guard and
// probe IsSupported() at runtime.
//
// Opus decoders carry state across packets (PLC and crossfade), so a
// single decoder shared across players would corrupt itself on packet
// interleave. The wrapper holds a fixed pool of `maxDecoders` decoder
// slots and binds an AudioPlayerId to a slot on first packet, evicting
// the least-recently-used slot when the pool is full. The encoder is a
// single instance (the local player).

#ifndef AUDIO_ENGINE_VOICE_OPUS_VOICE_CODEC_H
#define AUDIO_ENGINE_VOICE_OPUS_VOICE_CODEC_H

#include "audio_engine/voice_codec.h"

#include <cstdint>
#include <memory>

namespace audio {

class OpusVoiceCodec final : public IVoiceCodec {
public:
    struct Settings {
        // 8000, 12000, 16000, 24000, or 48000. 48 kHz is what Opus is
        // happiest with; the engine's voice path runs at this rate too.
        uint32_t sampleRate = 48000;
        uint32_t channels   = 1;          // mono voice; stereo is supported but bandwidth-heavy
        // Frames per packet. Must be one of: 120, 240, 480, 960, 1920, 2880
        // at 48 kHz (= 2.5, 5, 10, 20, 40, 60 ms). 20 ms (960) is the
        // game-voice-chat default; good latency / bandwidth trade.
        uint32_t frameSize  = 960;
        // Encoder bitrate in bits-per-second. 32 kbps is comfortable for
        // VoIP-grade voice; raise to 48–64 kbps for "production" speech
        // quality, drop to 16 kbps for bandwidth-constrained clients.
        int32_t  bitrateBps = 32000;
        // Maximum number of remote players this codec can decode in
        // parallel. The host should size this to its expected concurrent
        // voice-source count (typically the same as
        // AudioConfig.budget.maxVoiceSources).
        uint32_t maxDecoders = 16;
        // Use OPUS_APPLICATION_VOIP (true) or OPUS_APPLICATION_AUDIO
        // (false). VOIP optimises for speech intelligibility and
        // packet-loss robustness; AUDIO optimises for music quality.
        bool     applicationVoip = true;
    };

    // Default constructor; equivalent to OpusVoiceCodec(Settings{}) but
    // works around a C++ ordering quirk: a default argument like
    // `Settings settings = {}` on the explicit ctor below would require
    // Settings's default member initializers to be "complete" before the
    // enclosing class is, which they aren't.
    OpusVoiceCodec();
    explicit OpusVoiceCodec(const Settings& settings);
    ~OpusVoiceCodec() override;

    OpusVoiceCodec(const OpusVoiceCodec&)            = delete;
    OpusVoiceCodec& operator=(const OpusVoiceCodec&) = delete;

    // True when this build of the audio engine was compiled with
    // AUDIO_ENGINE_VOICE_OPUS=ON and libopus initialized successfully.
    // Hosts should check this before handing the codec to AudioRuntime;
    // an unsupported codec returns AudioResult::Unsupported on every
    // Encode/Decode call.
    bool IsSupported() const noexcept;

    const char* Name()       const noexcept override { return "opus"; }
    uint32_t    FrameSize()  const noexcept override;
    uint32_t    SampleRate() const noexcept override;
    uint32_t    Channels()   const noexcept override;

    AudioResult Decode(const VoicePacket& packet,
                        int16_t*           output,
                        uint32_t           outputCapacityFrames,
                        uint32_t&          outFrames) noexcept override;

    AudioResult DecodeLost(AudioPlayerId playerId,
                            int16_t*      output,
                            uint32_t      outputCapacityFrames,
                            uint32_t&     outFrames) noexcept override;

    AudioResult Encode(const int16_t* input,
                        uint32_t       frameCount,
                        uint8_t*       outBytes,
                        size_t         outCapacity,
                        size_t&        outSize) noexcept override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace audio

#endif // AUDIO_ENGINE_VOICE_OPUS_VOICE_CODEC_H
