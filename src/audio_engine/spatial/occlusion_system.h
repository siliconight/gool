// audio_engine/spatial/occlusion_system.h
//
// Per-frame, budgeted occlusion check. Each tick the system raycasts from
// the listener to up to N emitters (round-robin scheduling), updates each
// emitter's target occlusion, and smooths previous->target so per-frame
// transitions don't pop.
//
// Lives on the audio control thread. Reads listener position, writes per-
// emitter occlusion amount which is then mirrored into EmitterManager's SoA
// snapshot.

#ifndef AUDIO_ENGINE_SPATIAL_OCCLUSION_SYSTEM_H
#define AUDIO_ENGINE_SPATIAL_OCCLUSION_SYSTEM_H

#include <cstdint>
#include <vector>

#include "audio_engine/geometry_query.h"
#include "audio_engine/types.h"

namespace audio {

class OcclusionSystem {
public:
    OcclusionSystem(uint32_t maxEmitters,
                     uint32_t maxRaycastsPerFrame);

    void SetGeometryQuery(IAudioGeometryQuery* q) noexcept { geometry_ = q; }

    // Global intensity multiplier applied to per-emitter absorption +
    // damping. 1.0 = use raw values from the geometry query (physical
    // reality). <1 attenuates (gentler, more gameplay-friendly).
    // >1 exaggerates (clamped to [0,1] after multiplication, so very
    // absorbent materials saturate). Default 1.0 in the system; the
    // runtime pushes AudioConfig::occlusionIntensity here at init.
    // Safe to call at any time — the change applies on the next
    // budgeted raycast slice.
    void SetIntensity(float intensity) noexcept;

    // Reset all per-emitter state. Called on initialization or when emitters
    // are bulk-cleared.
    void Reset();

    // Update occlusion targets for the given emitter slots.
    // `emitterPositions` is indexed by emitter slot index [1, maxEmitters].
    // `slotOccupied` is non-zero where that slot is currently active. Updates
    // the smoothed `absorptionOut` and `dampingOut` in place. Both buffers
    // must be sized for [0, maxEmitters] (slot 0 is unused).
    //
    // The split between absorption and damping reflects what an
    // AudioOcclusionHit reports through ResolveOcclusion: absorption is
    // an overall gain reduction (volume cue), damping drives the per-
    // voice LPF (HF cue). Hosts that supply only the legacy
    // `materialAbsorption` field still get correct behavior because
    // ResolveOcclusion mirrors it to both.
    void Update(const Vec3&    listenerPos,
                 const Vec3*    emitterPositions,
                 const uint8_t* slotOccupied,
                 float*         absorptionOut,
                 float*         dampingOut,
                 float          deltaSeconds);

    uint32_t LastFrameRaycasts() const noexcept { return lastFrameRaycasts_; }

private:
    IAudioGeometryQuery* geometry_ = nullptr;
    uint32_t maxEmitters_      = 0;
    uint32_t maxRaycastsPerFrame_ = 0;
    uint32_t scheduleCursor_   = 1;     // next slot to raycast (1-based, slot 0 is null)
    float    intensity_        = 1.0f;  // SetIntensity; multiplies post-resolve

    // Per-emitter target absorption + damping. Index by slot.
    std::vector<float> targetAbsorption_;
    std::vector<float> targetDamping_;

    uint32_t lastFrameRaycasts_ = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_SPATIAL_OCCLUSION_SYSTEM_H
