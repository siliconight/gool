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

// audio_engine/decoders/memory_pcm_decoder.h
//
// A trivial IAudioDecoder over an interleaved float32 buffer. Used by tests
// and by RegisterStreamingFromMemory to wrap pre-decoded PCM in the same
// interface as a real codec. Not a public type.
//
// Two construction modes:
//   * Non-owning: `MemoryPcmDecoder(data, frames, sampleRate, channels)`;
//     caller guarantees the buffer outlives the decoder.
//   * Owning:    `MemoryPcmDecoder::CreateOwning(std::move(samples), ...)`;
//     decoder owns the buffer for its lifetime. Use this when the decoder
//     will outlive the caller's stack frame.
//
// Seek/0 rewinds; arbitrary seeks are supported.

#ifndef AUDIO_ENGINE_DECODERS_MEMORY_PCM_DECODER_H
#define AUDIO_ENGINE_DECODERS_MEMORY_PCM_DECODER_H

#include "audio_engine/decoders/audio_decoder.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace audio {

class MemoryPcmDecoder final : public IAudioDecoder {
public:
    // Non-owning. Caller guarantees `data` outlives this decoder.
    MemoryPcmDecoder(const float* data,
                      uint64_t     frames,
                      uint32_t     sampleRate,
                      uint32_t     channels) noexcept
        : data_(data),
          frames_(frames),
          sampleRate_(sampleRate),
          channels_(channels),
          cursor_(0) {}

    // Owning. Decoder takes the vector by move and points `data_` into it.
    static std::unique_ptr<MemoryPcmDecoder> CreateOwning(
        std::vector<float>&& samples,
        uint32_t             sampleRate,
        uint32_t             channels) {
        const uint32_t ch = (channels == 0) ? 1u : channels;
        const uint64_t fr = (samples.empty()) ? 0u : (samples.size() / ch);
        auto d = std::unique_ptr<MemoryPcmDecoder>(new MemoryPcmDecoder(
            nullptr, fr, sampleRate, ch));
        d->owned_ = std::move(samples);
        d->data_  = d->owned_.data();
        return d;
    }

    uint32_t SampleRate()  const noexcept override { return sampleRate_; }
    uint32_t Channels()    const noexcept override { return channels_; }
    uint64_t TotalFrames() const noexcept override { return frames_; }

    uint32_t DecodeFrames(float* out, uint32_t frames) noexcept override {
        if (!data_ || channels_ == 0 || cursor_ >= frames_) return 0;
        const uint64_t remaining = frames_ - cursor_;
        const uint32_t take      = static_cast<uint32_t>(
            (remaining < frames) ? remaining : frames);
        const uint32_t total     = take * channels_;
        const float*   src       = data_ + cursor_ * channels_;
        for (uint32_t i = 0; i < total; ++i) out[i] = src[i];
        cursor_ += take;
        return take;
    }

    bool Seek(uint64_t frame) noexcept override {
        if (frame > frames_) return false;
        cursor_ = frame;
        return true;
    }

private:
    const float*       data_;
    uint64_t           frames_;
    uint32_t           sampleRate_;
    uint32_t           channels_;
    uint64_t           cursor_;
    std::vector<float> owned_;     // empty for non-owning instances
};

} // namespace audio

#endif // AUDIO_ENGINE_DECODERS_MEMORY_PCM_DECODER_H
