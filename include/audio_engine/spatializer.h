// audio_engine/spatializer.h
//
// Spatializer seam. Reads per-frame inputs (one (emitter × listener) pair
// at a time, fed from EmitterManager's SoA mirror) and produces the per-
// source mix parameters consumed by AudioMixer.
//
// One of the four real polymorphism seams. Default implementation in
// src/audio_engine/spatial/default_spatializer.{h,cpp}. HRTF, vendor
// spatializers can be plugged in by the host via AudioRuntimeDependencies.

#ifndef AUDIO_ENGINE_SPATIALIZER_H
#define AUDIO_ENGINE_SPATIALIZER_H

#include "audio_engine/types.h"

namespace audio {

// Output of the spatializer: what the mixer needs to shape one source.
//
// Two output modes coexist. By default the spatializer produces (gain, pan,
// lpfAmount, reverbSend) and the mixer applies equal-power pan with a single
// shared low-pass filter. When `useBinaural` is true the mixer ignores
// `pan` / `lowPassAmount` and uses the per-ear fields instead, applying:
//
//   1. independent gainL / gainR             (ILD: head-shadow level cue)
//   2. fractional per-ear delay              (ITD: which ear hears it first)
//   3. independent lpfAmountL / lpfAmountR   (head-shadow HF rolloff cue)
//
// `useBinaural=false` is the path DefaultSpatializer drives.
// `useBinaural=true`  is what SphericalHeadSpatializer drives.
struct SpatialParams {
    float gain          = 1.0f;
    float pan           = 0.0f;     // -1 = full left, 0 = center, +1 = full right
    float pitch         = 1.0f;
    float lowPassAmount = 0.0f;     // 0 = off, 1 = fully damped
    float reverbSend    = 0.0f;     // 0..1 wet send level

    // Per-ear (binaural) fields. Active only when `useBinaural` is true.
    bool  useBinaural    = false;
    float gainL          = 1.0f;
    float gainR          = 1.0f;
    float delaySamplesL  = 0.0f;    // fractional; only one ear is non-zero
    float delaySamplesR  = 0.0f;
    float lpfAmountL     = 0.0f;
    float lpfAmountR     = 0.0f;
};

// The per-frame, per-emitter inputs the spatializer reads. Populated from
// EmitterManager's SoA snapshot. Fields are exactly those needed in the
// per-pair hot loop; lifecycle/registry data lives elsewhere.
struct SpatialEmitterView {
    Vec3          position{};
    Vec3          velocity{};
    float         occlusionAmount  = 0.0f;   // 0 = clear, 1 = fully occluded (volume cue)
    float         occlusionDamping = 0.0f;   // 0 = no HF cut, 1 = heavy muffle
    float         minDistance      = 1.0f;
    float         maxDistance      = 40.0f;
    float         volumeFloor      = 0.0f;
    FalloffModel  falloffModel     = FalloffModel::Logarithmic;
    AudioCategory category         = AudioCategory::SFX;
    bool          spatialized      = true;
};

struct SpatialListenerView {
    Vec3 position{};
    Vec3 forward{0.0f, 0.0f, -1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 velocity{};
};

struct SpatialEnvironmentState {
    float globalReverbSend     = 0.0f;
    float globalLowPass        = 0.0f;
    float speedOfSound         = 343.0f;
    bool  dopplerEnabled       = true;
    // Air absorption: lowPassAmount contribution per meter of distance to
    // the listener. Default 0 keeps the spatializer compatible with hosts
    // that don't enable it. The runtime sets this from
    // AudioConfig.airAbsorptionPerMeter when AudioConfig.enableAirAbsorption
    // is true.
    float airAbsorptionPerMeter = 0.0f;
};

class ISpatializer {
public:
    virtual ~ISpatializer() = default;

    virtual SpatialParams Calculate(
        const SpatialEmitterView&     emitter,
        const SpatialListenerView&    listener,
        const SpatialEnvironmentState& environment) const noexcept = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_SPATIALIZER_H
