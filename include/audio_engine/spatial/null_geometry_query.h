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

// audio_engine/spatial/null_geometry_query.h
//
// Default geometry query: never reports an occluding hit. Engine falls back
// to this if the host does not supply an IAudioGeometryQuery via
// AudioRuntimeDependencies.

#ifndef AUDIO_ENGINE_SPATIAL_NULL_GEOMETRY_QUERY_H
#define AUDIO_ENGINE_SPATIAL_NULL_GEOMETRY_QUERY_H

#include "audio_engine/geometry_query.h"

namespace audio {

class NullGeometryQuery final : public IAudioGeometryQuery {
public:
    bool RaycastAudioOcclusion(const Vec3& /*from*/,
                                const Vec3& /*to*/,
                                AudioOcclusionHit& outHit) noexcept override {
        outHit = {};
        return false;
    }
};

} // namespace audio

#endif // AUDIO_ENGINE_SPATIAL_NULL_GEOMETRY_QUERY_H
