// audio_engine/dsp/gain_effect.cpp

#include "audio_engine/dsp/gain_effect.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

float DbToLinear(float dB) noexcept {
    return std::pow(10.0f, dB * 0.05f);
}

} // namespace

GainEffect::GainEffect(float initialGainDb) {
    target_  = DbToLinear(initialGainDb);
    current_ = target_;
}

void GainEffect::Prepare(uint32_t sampleRate, uint32_t /*channels*/) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
}

void GainEffect::Process(float* output, uint32_t frames, uint32_t channels,
                         const float* /*sidechain*/, uint32_t /*sidechainChannels*/) noexcept {
    if (channels == 0 || frames == 0) return;

    // Fast path: target reached, no ramp.
    if (std::abs(target_ - current_) < 1e-6f) {
        current_ = target_;
        if (std::abs(current_ - 1.0f) < 1e-6f) return;     // unity gain
        const float g = current_;
        const uint32_t total = frames * channels;
        for (uint32_t i = 0; i < total; ++i) output[i] *= g;
        return;
    }

    // Ramp current toward target over kRampMs of audio at the configured rate.
    const float rampSamples = std::max(1.0f, kRampMs * 0.001f * static_cast<float>(sampleRate_));
    const float step        = (target_ - current_) / rampSamples;

    float g = current_;
    for (uint32_t f = 0; f < frames; ++f) {
        // Snap to target when we cross it.
        if ((step > 0.0f && g >= target_) || (step < 0.0f && g <= target_)) {
            g = target_;
        }
        for (uint32_t c = 0; c < channels; ++c) {
            output[f * channels + c] *= g;
        }
        g += step;
    }
    current_ = (step > 0.0f) ? std::min(g, target_) : std::max(g, target_);
}

void GainEffect::OnParameter(uint16_t paramId, float value) noexcept {
    if (paramId == EffectParameter::Gain_GainDb) {
        target_ = DbToLinear(value);
    }
}

} // namespace audio
