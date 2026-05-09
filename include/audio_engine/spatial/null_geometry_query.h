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
