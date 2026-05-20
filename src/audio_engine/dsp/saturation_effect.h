// audio_engine/dsp/saturation_effect.h
//
// Single-stage tanh waveshaper with first-order Antiderivative
// Anti-Aliasing (ADAA). Soft, smooth, "analog-ish" saturation for
// subtle bus glue and impact reinforcement — NOT a full multi-mode
// distortion engine. The intent is light enhancement (drive 1.5–2.5,
// mix 0.10–0.30) on top of whatever the sound designers already shaped
// in their DAW. If you need aggressive distortion, do it offline.
//
//   Topology:
//
//       x[n] = (dry[n] + bias) * drive
//       f(x) = tanh(x)
//       F(x) = log(cosh(x))             (first antiderivative of tanh)
//
//                  F(x[n]) - F(x[n-1])
//       y[n]  =  ─────────────────────       (when |x[n] - x[n-1]| ≥ ε)
//                     x[n] - x[n-1]
//
//       y[n]  =  f((x[n] + x[n-1]) / 2)      (midpoint fallback, |diff| < ε)
//
//       wet   = (y[n] - tanh(bias * drive)) * outputGain
//       out   = dry * (1 - mix) + wet * mix
//
//   * `drive`        - pre-shaper input gain. > 1 generates harmonics.
//                      1.0 = essentially linear pass-through. Typical 1.5–4.0.
//   * `mix`          - parallel dry/wet blend. 0 = bypass (default,
//                      makes adding the effect to a bus a no-op until
//                      you turn it up). 1 = fully wet. Subtle glue
//                      lives in 0.10–0.30.
//   * `outputGain`   - post-shaper gain trim. Use to compensate for
//                      the loudness change that drive introduces; a
//                      good rule of thumb is ~ 1.0/sqrt(drive).
//   * `bias`         - DC offset added before shaping, then DC-corrected
//                      out of the wet path. 0 = symmetric (odd
//                      harmonics only); non-zero introduces even
//                      harmonics — "tube"/"warmth" character. Typical
//                      0.05–0.20.
//
// Per-channel state: one double (`x[n-1]`) per channel. Allocated in
// Prepare() and zero-initialized. No envelope follower, no ring
// buffers. Parameter changes are instantaneous (no internal slew);
// for smooth live automation, drive these from the host side or layer
// a Gain effect in front to ramp the signal level.
//
// Anti-aliasing (v0.38.0):
//   First-order ADAA per Parker, Zavalishin, Le Bivic, DAFX 2016
//   (the canonical reference; see docs/audio_design/saturation_v2.md
//   for full design rationale). At drive ≥ 2.5 on transient-rich
//   sources (cymbals, fricatives, gunshot clicks) the previous
//   non-bandlimited tanh introduced foldover aliasing well above the
//   noise floor; ADAA reduces aliasing-band energy by ~40 dB on a
//   19 kHz test tone at 48 kHz sample rate.
//
//   Cost: ~3× a raw tanh per active sample (the F(x) = log(cosh(x))
//   antiderivative dominates, transcendental). The mix == 0 bypass
//   path is unchanged and remains essentially free.
//
//   Tradeoffs: half-sample group delay (inaudible; not even worth
//   compensating since the wet path is summed with a dry path that
//   would itself have to be delayed to null). Output at drive ≈ 1.0
//   is no longer bit-identical to plain tanh — within ~1e-4 absolute
//   on typical input, which is below float quantization.

#ifndef AUDIO_ENGINE_DSP_SATURATION_EFFECT_H
#define AUDIO_ENGINE_DSP_SATURATION_EFFECT_H

#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/bus.h"

#include <vector>

namespace audio {

struct SaturationConfig {
    float drive      = 1.0f;
    float mix        = 0.0f;     // default-off so adding to a bus is a no-op
    float outputGain = 1.0f;
    float bias       = 0.0f;
};

class SaturationEffect final : public IDspEffect {
public:
    explicit SaturationEffect(const SaturationConfig& cfg);

    void Prepare(uint32_t sampleRate, uint32_t channels) override;
    void Process(float* output, uint32_t frames, uint32_t channels,
                 const float* sidechain, uint32_t sidechainChannels) noexcept override;
    void OnParameter(uint16_t paramId, float value) noexcept override;
    uint16_t SidechainBusId() const noexcept override { return kInvalidBusId; }
    // v0.28.0: introspection.
    EffectKind Kind() const noexcept override { return EffectKind::Saturation; }
    float GetParameter(uint16_t paramId) const noexcept override;

    // Diagnostics.
    float Drive()      const noexcept { return drive_; }
    float Mix()        const noexcept { return mix_; }
    float OutputGain() const noexcept { return outputGain_; }
    float Bias()       const noexcept { return bias_; }

private:
    float drive_;
    float mix_;
    float outputGain_;
    float bias_;

    // v0.38.0: per-channel ADAA state — the previous post-drive
    // sample value x[n-1] = (dry[n-1] + bias) * drive. Resized in
    // Prepare(); not touched when mix_ == 0 (the bypass path), so a
    // mix=0 → mix>0 transition may produce one sample of staleness
    // before steady-state ADAA. Held in double precision because the
    // (F(x) - F(x_prev)) / (x - x_prev) divide is ill-conditioned in
    // float near the midpoint-fallback threshold.
    std::vector<double> prevDriven_;
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_SATURATION_EFFECT_H
