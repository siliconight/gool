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

// audio_engine/handles.h
//
// Generation-counted handle types. Stale handles to recycled slots fail
// closed (Get() returns nullptr) rather than silently addressing a different
// resource. Index 0 is reserved as the null slot; default construction
// produces a null handle.

#ifndef AUDIO_ENGINE_HANDLES_H
#define AUDIO_ENGINE_HANDLES_H

#include <cstdint>

namespace audio {

struct EmitterHandle {
    uint32_t index = 0;
    uint32_t generation = 0;

    constexpr bool IsNull() const noexcept {
        return index == 0 && generation == 0;
    }
    constexpr bool operator==(const EmitterHandle&) const noexcept = default;
};

struct VoiceSourceHandle {
    uint32_t index = 0;
    uint32_t generation = 0;

    constexpr bool IsNull() const noexcept {
        return index == 0 && generation == 0;
    }
    constexpr bool operator==(const VoiceSourceHandle&) const noexcept = default;
};

struct AudioAssetHandle {
    uint32_t index = 0;
    uint32_t generation = 0;

    constexpr bool IsNull() const noexcept {
        return index == 0 && generation == 0;
    }
    constexpr bool operator==(const AudioAssetHandle&) const noexcept = default;
};

constexpr EmitterHandle     kNullEmitterHandle{};
constexpr VoiceSourceHandle kNullVoiceSourceHandle{};
constexpr AudioAssetHandle  kNullAudioAssetHandle{};

} // namespace audio

#endif // AUDIO_ENGINE_HANDLES_H
