// audio_engine/spatial/spherical_head_spatializer.h
//
// Binaural spatializer driving the mixer's per-ear path. Computes ITD
// from Woodworth's spherical-head formula and ILD from a simple
// head-shadow approximation; sets useBinaural=true on the SpatialParams
// it returns.
//
// What this gives you that DefaultSpatializer doesn't:
//   * Real horizontal-plane localization (ear-time-difference up to
//     ~0.65 ms, the dominant directional cue below 1.5 kHz).
//   * Frequency-dependent head shadowing (the HF cue above 1.5 kHz).
//
// What this still doesn't give you:
//   * Pinna spectral notches (elevation, front/back disambiguation).
//     Those require measured HRIR data and a separate convolution pass;
//     this implementation is the seam where that would plug in next.
//
// All other SpatialParams fields (gain, pitch, reverbSend) are computed
// the same way DefaultSpatializer computes them, including the existing
// distance attenuation, occlusion, air absorption, and Doppler logic.
// Only the directional cue (pan + lpfAmount) is replaced.

#ifndef AUDIO_ENGINE_SPATIAL_SPHERICAL_HEAD_SPATIALIZER_H
#define AUDIO_ENGINE_SPATIAL_SPHERICAL_HEAD_SPATIALIZER_H

#include "audio_engine/spatializer.h"

namespace audio {

class SphericalHeadSpatializer final : public ISpatializer {
public:
    struct Settings {
        // Effective head radius in meters. 0.0875 is the Kemar-dummy
        // value used in most academic literature; smaller heads (kids,
        // small adults) localise with slightly less ITD.
        float headRadiusMeters = 0.0875f;

        // Maximum extra HF damping applied to the contralateral ear at
        // full shadow. The biquad LPF inside the mixer interprets this
        // 0..1 amount the same way the occlusion path does.
        float maxShadowLpfAmount = 0.55f;

        // Maximum overall gain reduction (linear) applied to the
        // contralateral ear at full shadow. ~0.6 corresponds to ~-4.4
        // dB, which lines up with measured ILD at the upper-mid bands
        // for a sound at 90° azimuth.
        float minShadowGain = 0.6f;
    };

    SphericalHeadSpatializer() = default;
    explicit SphericalHeadSpatializer(Settings s) : settings_(s) {}

    SpatialParams Calculate(
        const SpatialEmitterView&     emitter,
        const SpatialListenerView&    listener,
        const SpatialEnvironmentState& environment) const noexcept override;

private:
    Settings settings_{};
};

} // namespace audio

#endif // AUDIO_ENGINE_SPATIAL_SPHERICAL_HEAD_SPATIALIZER_H
