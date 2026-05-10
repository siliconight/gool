// audio_engine/dsp/saturation_effect.h
//
// Single-stage tanh waveshaper. Soft, smooth, "analog-ish" saturation
// for subtle bus glue and impact reinforcement — NOT a full multi-mode
// distortion engine. The intent is light enhancement (drive 1.5–2.5,
// mix 0.10–0.30) on top of whatever the sound designers already shaped
// in their DAW. If you need aggressive distortion, do it offline.
//
//   Topology:
//
//       wet  = (tanh((dry + bias) * drive) - tanh(bias * drive))
//                  * outputGain
//       out  = dry * (1 - mix) + wet * mix
//
//   * `drive`        - pre-shaper input gain. > 1 generates harmonics.
//                      1.0 = no harmonics at all (linear pass-through if
//                      bias=0). Typical 1.5–4.0.
//   * `mix`          - parallel dry/wet blend. 0 = bypass (default,
//                      makes adding the effect to a bus a no-op until
//                      you turn it up). 1 = fully wet. Subtle glue
//                      lives in 0.10–0.30.
//   * `outputGain`   - post-shaper gain trim. Use to compensate for
//                      the loudness change that drive introduces; a
//                      good rule of thumb is ~ 1.0/sqrt(drive).
//   * `bias`         - DC offset added before shaping, then subtracted
//                      from output (DC-corrected). 0 = symmetric tanh
//                      (odd harmonics only). Non-zero introduces even
//                      harmonics — "tube"/"warmth" character. Typical
//                      0.05–0.20.
//
// Stateless per-sample. No envelope follower, no ring buffers, no
// allocations. Parameter changes are instantaneous (no internal
// slew). For smooth live automation, drive these from the host side
// or layer a Gain effect in front to ramp the signal level.
//
// Aliasing note:
//   No oversampling. tanh introduces harmonics above Nyquist that
//   fold back as aliasing — at typical drive values (≤ 4.0) on
//   typical game-audio source material this is well below noise
//   floor and effectively inaudible. Push drive much higher on
//   bright, transient-rich sources (cymbals, fricatives) and
//   aliasing becomes audible. The textbook fix is a 2× polyphase
//   upsampler around the waveshaper; not shipped here pending
//   profile data showing real demand.

#ifndef AUDIO_ENGINE_DSP_SATURATION_EFFECT_H
#define AUDIO_ENGINE_DSP_SATURATION_EFFECT_H

#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/bus.h"

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
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_SATURATION_EFFECT_H
