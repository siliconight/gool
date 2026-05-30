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

// audio_engine/decoders/audio_decoder.h
//
// Internal interface over a PCM source. The asset registry uses a decoder
// during one-shot file load; the streaming pump uses one for the lifetime
// of a streaming voice.
//
// Conventions:
//   * DecodeFrames returns the number of *frames* (not samples) written into
//     `out`. A return less than `frames` signals EOF reached during this
//     call. Subsequent calls return 0.
//   * Output is interleaved float32 in [-1, 1].
//   * Channels() and SampleRate() are constant after construction.
//   * TotalFrames() returns 0 if unknown (some Ogg streams).
//   * Seek(0) is the canonical "rewind for loop" operation. Returns false
//     if the underlying source is not seekable.

#ifndef AUDIO_ENGINE_DECODERS_AUDIO_DECODER_H
#define AUDIO_ENGINE_DECODERS_AUDIO_DECODER_H

#include <cstdint>

namespace audio {

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    virtual uint32_t SampleRate()  const noexcept = 0;
    virtual uint32_t Channels()    const noexcept = 0;
    virtual uint64_t TotalFrames() const noexcept = 0;

    virtual uint32_t DecodeFrames(float* out, uint32_t frames) noexcept = 0;

    virtual bool Seek(uint64_t frame) noexcept = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_DECODERS_AUDIO_DECODER_H
