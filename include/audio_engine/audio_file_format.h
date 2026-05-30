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

// audio_engine/audio_file_format.h
//
// Format hint for in-memory decode entry points. `Auto` triggers magic-byte
// sniffing; the explicit values short-circuit detection when the host
// already knows what it has.

#ifndef AUDIO_ENGINE_AUDIO_FILE_FORMAT_H
#define AUDIO_ENGINE_AUDIO_FILE_FORMAT_H

#include <cstdint>

namespace audio {

enum class AudioFileFormat : uint8_t {
    Auto = 0,
    Wav,
    OggVorbis,
    Flac,
    Opus,
};

} // namespace audio

#endif // AUDIO_ENGINE_AUDIO_FILE_FORMAT_H
