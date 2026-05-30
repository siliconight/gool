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

// audio_engine/decoders/decoder_factory.h
//
// Format detection and decoder construction. All members are static; no
// state. Returns nullptr on unknown / unsupported / malformed input; the
// caller maps that to the appropriate AudioResult.

#ifndef AUDIO_ENGINE_DECODERS_DECODER_FACTORY_H
#define AUDIO_ENGINE_DECODERS_DECODER_FACTORY_H

#include "audio_engine/audio_file_format.h"
#include "audio_engine/decoders/audio_decoder.h"

#include <cstdint>
#include <memory>

namespace audio {

class DecoderFactory {
public:
    // Detect format from the first few bytes. Returns AudioFileFormat::Auto
    // when the magic doesn't match anything we know.
    static AudioFileFormat SniffFormat(const uint8_t* data, uint64_t size) noexcept;

    // Detect format from a path's extension. Returns Auto on no/unknown ext.
    static AudioFileFormat FormatFromExtension(const char* path) noexcept;

    static std::unique_ptr<IAudioDecoder> CreateForFile(const char* path);

    static std::unique_ptr<IAudioDecoder> CreateForMemory(const uint8_t* data,
                                                            uint64_t       size,
                                                            AudioFileFormat hint
                                                                = AudioFileFormat::Auto);
};

} // namespace audio

#endif // AUDIO_ENGINE_DECODERS_DECODER_FACTORY_H
