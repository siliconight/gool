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

// audio_engine/decoders/flac_decoder.h
//
// IAudioDecoder over FLAC files via dr_flac. Compiled when
// AUDIO_ENGINE_DECODERS_FLAC is defined.

#ifndef AUDIO_ENGINE_DECODERS_FLAC_DECODER_H
#define AUDIO_ENGINE_DECODERS_FLAC_DECODER_H

#include "audio_engine/decoders/audio_decoder.h"

#include <cstdint>
#include <memory>

namespace audio {

class FlacDecoder final : public IAudioDecoder {
public:
    static std::unique_ptr<FlacDecoder> CreateFromFile(const char* path);
    static std::unique_ptr<FlacDecoder> CreateFromMemory(const uint8_t* data,
                                                          uint64_t       size);

    ~FlacDecoder() override;

    uint32_t SampleRate()  const noexcept override { return sampleRate_; }
    uint32_t Channels()    const noexcept override { return channels_; }
    uint64_t TotalFrames() const noexcept override { return totalFrames_; }

    uint32_t DecodeFrames(float* out, uint32_t frames) noexcept override;
    bool     Seek(uint64_t frame) noexcept override;

private:
    FlacDecoder() = default;

    struct State;
    std::unique_ptr<State> state_;

    uint32_t sampleRate_  = 0;
    uint32_t channels_    = 0;
    uint64_t totalFrames_ = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_DECODERS_FLAC_DECODER_H
