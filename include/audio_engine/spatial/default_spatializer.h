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
