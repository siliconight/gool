// audio_engine/spatial/default_spatializer.h
//
// Default spatializer. Combines distance attenuation, equal-power pan,
// occlusion gain + LPF amount, distance-driven air absorption, Doppler
// pitch ratio, and the global reverb send into the SpatialParams the
// mixer applies to a voice. No HRTF or per-ear delay; those would
// arrive as a separate ISpatializer implementation.
//
// Stateless: safe to call from any thread, including the render thread.
// In practice the runtime calls it from the audio control thread during
// Update() while building the mixer command stream.

#ifndef AUDIO_ENGINE_SPATIAL_DEFAULT_SPATIALIZER_H
#define AUDIO_ENGINE_SPATIAL_DEFAULT_SPATIALIZER_H

#include "audio_engine/spatializer.h"

namespace audio {

class DefaultSpatializer final : public ISpatializer {
public:
    SpatialParams Calculate(
        const SpatialEmitterView&     emitter,
        const SpatialListenerView&    listener,
        const SpatialEnvironmentState& environment) const noexcept override;
};

} // namespace audio

#endif // AUDIO_ENGINE_SPATIAL_DEFAULT_SPATIALIZER_H
