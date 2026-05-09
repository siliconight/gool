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
