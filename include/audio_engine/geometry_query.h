// audio_engine/geometry_query.h
//
// Geometry query seam. The audio engine does not own physics; the host
// supplies an implementation that delegates to its physics system. One
// of the four real polymorphism seams. NullGeometryQuery is the default
// fallback and always reports "no hit".
//
// Calls happen on the audio control thread (never the render thread) and
// are budgeted per Update tick.

#ifndef AUDIO_ENGINE_GEOMETRY_QUERY_H
#define AUDIO_ENGINE_GEOMETRY_QUERY_H

#include "audio_engine/types.h"
#include <algorithm>

namespace audio {

// Coarse material classification. The host returns one of these from a
// raycast hit (typically by mapping its own material/surface tags) and
// the engine applies sensible defaults for both absorption (overall
// gain reduction) and damping (high-frequency rolloff). Hosts that want
// finer control can ignore this and write `absorption` / `damping`
// directly on AudioOcclusionHit.
enum class AudioMaterial : uint8_t {
    Default     = 0,    // unknown; treat as a moderately damping wood-like surface
    Air         = 1,    // pass-through; same as no hit
    Glass       = 2,    // mostly transparent, slight HF cut
    Wood        = 3,
    Drywall     = 4,
    Concrete    = 5,    // strong both
    Metal       = 6,    // strong absorption, less damping (rings more)
    Curtain     = 7,    // strong damping, modest absorption
    Foliage     = 8,    // diffuse, mostly damping
};

struct AudioOcclusionHit {
    bool  hit                 = false;
    Vec3  hitPoint{};
    float distance            = 0.0f;

    // Decoupled occlusion components. The legacy `materialAbsorption`
    // field below is mapped onto both at the spatializer when `damping`
    // is left at its sentinel of -1.0; new code should set both
    // explicitly (or set `material` and let the engine fill them in).
    //
    //   absorption  0..1  overall gain reduction (volume cue)
    //   damping     0..1  contribution to the per-voice LPF (HF cue)
    //
    // A curtain has high damping but modest absorption (the highs die
    // but you still hear the level). Concrete has both. Glass has
    // neither, much. Modeling these independently is what lets a
    // designer tune "soft" vs "hard" surfaces.
    float absorption          = 0.0f;
    float damping             = -1.0f;   // -1 = inherit from absorption

    // Material preset, optional. If `material != Default`, the
    // spatializer maps it to absorption + damping defaults below
    // (overriding any explicit values). Hosts choose either path.
    AudioMaterial material    = AudioMaterial::Default;

    // Legacy field. Hosts that still set only this leave the new fields
    // at defaults; the spatializer treats `materialAbsorption` as
    // setting both `absorption` and `damping` to that value, preserving
    // pre-existing behavior bit-for-bit.
    float materialAbsorption  = 0.0f;
};

// Map an AudioMaterial preset to (absorption, damping) defaults. The
// values are deliberately conservative: a host that wants more
// aggressive occlusion can scale them in their own raycast wrapper.
inline void AudioMaterialDefaults(AudioMaterial m,
                                    float& absorption,
                                    float& damping) noexcept {
    switch (m) {
        case AudioMaterial::Air:      absorption = 0.00f; damping = 0.00f; break;
        case AudioMaterial::Glass:    absorption = 0.10f; damping = 0.05f; break;
        case AudioMaterial::Wood:     absorption = 0.50f; damping = 0.40f; break;
        case AudioMaterial::Drywall:  absorption = 0.55f; damping = 0.55f; break;
        case AudioMaterial::Concrete: absorption = 0.90f; damping = 0.75f; break;
        case AudioMaterial::Metal:    absorption = 0.70f; damping = 0.30f; break;
        case AudioMaterial::Curtain:  absorption = 0.30f; damping = 0.80f; break;
        case AudioMaterial::Foliage:  absorption = 0.35f; damping = 0.60f; break;
        case AudioMaterial::Default:
        default:                      absorption = 0.50f; damping = 0.50f; break;
    }
}

// Resolve the effective (absorption, damping) pair from an
// AudioOcclusionHit, honoring the precedence:
//   1. material != Default     -> AudioMaterialDefaults(material)
//   2. damping >= 0            -> use explicit absorption + damping
//   3. otherwise (legacy)      -> absorption = damping = materialAbsorption
inline void ResolveOcclusion(const AudioOcclusionHit& h,
                                float& outAbsorption,
                                float& outDamping) noexcept {
    if (h.material != AudioMaterial::Default) {
        AudioMaterialDefaults(h.material, outAbsorption, outDamping);
        return;
    }
    if (h.damping >= 0.0f) {
        outAbsorption = std::clamp(h.absorption, 0.0f, 1.0f);
        outDamping    = std::clamp(h.damping,    0.0f, 1.0f);
        return;
    }
    const float v = std::clamp(h.materialAbsorption, 0.0f, 1.0f);
    outAbsorption = v;
    outDamping    = v;
}

class IAudioGeometryQuery {
public:
    virtual ~IAudioGeometryQuery() = default;

    // Synchronous raycast from `from` to `to`. Implementations must be
    // thread-safe with respect to the host's physics scene; the engine calls
    // this from the audio control thread.
    virtual bool RaycastAudioOcclusion(
        const Vec3&        from,
        const Vec3&        to,
        AudioOcclusionHit& outHit) noexcept = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_GEOMETRY_QUERY_H
