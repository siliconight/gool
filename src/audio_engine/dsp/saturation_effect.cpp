// audio_engine/dsp/saturation_effect.cpp
//
// See saturation_effect.h for topology overview.

#include "audio_engine/dsp/saturation_effect.h"

#include <algorithm>
#include <cmath>

namespace audio {

SaturationEffect::SaturationEffect(const SaturationConfig& cfg)
    : drive_(std::max(0.0f, cfg.drive)),
      mix_(std::clamp(cfg.mix, 0.0f, 1.0f)),
      outputGain_(std::max(0.0f, cfg.outputGain)),
      bias_(std::clamp(cfg.bias, -1.0f, 1.0f)) {}

void SaturationEffect::Prepare(uint32_t /*sampleRate*/, uint32_t /*channels*/) {
    // Stateless waveshaper — nothing to allocate or precompute against
    // sample rate. Kept as an empty hook so the interface contract
    // (Prepare may allocate; called once at Initialize) is honored.
}

void SaturationEffect::Process(float* output, uint32_t frames, uint32_t channels,
                                 const float* /*sidechain*/, uint32_t /*sidechainChannels*/) noexcept {
    if (channels == 0 || frames == 0) return;

    // Bypass when fully dry — saves the per-sample tanh on every bus
    // that has the effect installed but turned off (the default
    // SaturationConfig leaves mix at 0). This is the "free if unused"
    // case; we want it cheap because hosts will leave the effect
    // installed and modulate mix from gameplay code.
    if (mix_ <= 0.0f) return;

    // Snapshot parameter values to locals — OnParameter writes to the
    // underscore-suffixed members, but Process reads the snapshot. Two
    // benefits: (a) one parameter mid-buffer change can't half-process
    // a frame with stale drive and fresh mix, (b) the compiler can
    // hoist the constants out of the inner loop.
    const float drive    = drive_;
    const float bias     = bias_;
    const float mix      = mix_;
    const float outG     = outputGain_;
    const float invMix   = 1.0f - mix;

    // DC removal: when bias != 0, the output of tanh(bias*drive) is a
    // constant DC offset added to every sample. Subtract it once,
    // outside the loop, so the wet signal averages around zero
    // regardless of bias setting.
    const float dcRemove = (bias != 0.0f) ? std::tanh(bias * drive) : 0.0f;

    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < channels; ++c) {
            const uint32_t idx = f * channels + c;
            const float dry    = output[idx];
            const float driven = (dry + bias) * drive;
            const float wet    = (std::tanh(driven) - dcRemove) * outG;
            output[idx]        = dry * invMix + wet * mix;
        }
    }
}

void SaturationEffect::OnParameter(uint16_t paramId, float value) noexcept {
    switch (paramId) {
        case EffectParameter::Saturation_Drive:
            drive_ = std::max(0.0f, value);
            break;
        case EffectParameter::Saturation_Mix:
            mix_ = std::clamp(value, 0.0f, 1.0f);
            break;
        case EffectParameter::Saturation_OutputGain:
            outputGain_ = std::max(0.0f, value);
            break;
        case EffectParameter::Saturation_Bias:
            bias_ = std::clamp(value, -1.0f, 1.0f);
            break;
        default:
            break;
    }
}

// v0.28.0: introspection — mirror of OnParameter.
float SaturationEffect::GetParameter(uint16_t paramId) const noexcept {
    switch (paramId) {
        case EffectParameter::Saturation_Drive:      return drive_;
        case EffectParameter::Saturation_Mix:        return mix_;
        case EffectParameter::Saturation_OutputGain: return outputGain_;
        case EffectParameter::Saturation_Bias:       return bias_;
        default:                                     return 0.0f;
    }
}

} // namespace audio
