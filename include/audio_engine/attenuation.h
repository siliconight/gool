// audio_engine/attenuation.h
//
// Distance attenuation: how source loudness drops with distance from the
// listener. The math runs in the spatializer's hot loop; keep it inlinable
// and branch-light.

#ifndef AUDIO_ENGINE_ATTENUATION_H
#define AUDIO_ENGINE_ATTENUATION_H

#include "audio_engine/types.h"

namespace audio {

struct AttenuationSettings {
    float minDistance = 1.0f;       // gain == 1.0 below this distance
    float maxDistance = 40.0f;      // gain == volumeFloor at and beyond
    float volumeFloor = 0.0f;
    FalloffModel falloffModel = FalloffModel::Logarithmic;
};

// Compute the linear gain multiplier [volumeFloor, 1.0] for a source at the
// given distance under the configured falloff model. Pure function; safe to
// call from any thread including the render thread.
float ComputeAttenuationGain(const AttenuationSettings& s, float distance) noexcept;

} // namespace audio

#endif // AUDIO_ENGINE_ATTENUATION_H
