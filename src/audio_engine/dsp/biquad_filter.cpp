// audio_engine/dsp/biquad_filter.cpp

#include "audio_engine/dsp/biquad_filter.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

BiquadFilterEffect::BiquadFilterEffect(BiquadType type, float cutoffHz, float Q)
    : type_(type), cutoffHz_(cutoffHz), Q_(Q) {}

void BiquadFilterEffect::Prepare(uint32_t sampleRate, uint32_t /*channels*/) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    dirty_      = true;
    z1_.fill(0.0f);
    z2_.fill(0.0f);
}

void BiquadFilterEffect::RecomputeCoefficients() noexcept {
    // Clamp cutoff to Nyquist - epsilon to keep tan() stable.
    const float fs    = static_cast<float>(sampleRate_);
    const float fc    = std::clamp(cutoffHz_, 10.0f, 0.49f * fs);
    const float Q     = std::max(0.1f, Q_);
    const float omega = 2.0f * kPi * fc / fs;
    const float sn    = std::sin(omega);
    const float cs    = std::cos(omega);
    const float alpha = sn / (2.0f * Q);

    // Initialize defensively in case BiquadType gains a new value later.
    float a0  = 1.0f;
    float b0u = 1.0f, b1u = 0.0f, b2u = 0.0f, a1u = 0.0f, a2u = 0.0f;

    switch (type_) {
        case BiquadType::LowPass: {
            b0u = (1.0f - cs) * 0.5f;
            b1u = 1.0f - cs;
            b2u = (1.0f - cs) * 0.5f;
            a0  = 1.0f + alpha;
            a1u = -2.0f * cs;
            a2u = 1.0f - alpha;
        } break;
        case BiquadType::HighPass: {
            b0u = (1.0f + cs) * 0.5f;
            b1u = -(1.0f + cs);
            b2u = (1.0f + cs) * 0.5f;
            a0  = 1.0f + alpha;
            a1u = -2.0f * cs;
            a2u = 1.0f - alpha;
        } break;
        case BiquadType::BandPass: {
            b0u = alpha;
            b1u = 0.0f;
            b2u = -alpha;
            a0  = 1.0f + alpha;
            a1u = -2.0f * cs;
            a2u = 1.0f - alpha;
        } break;
    }

    const float invA0 = 1.0f / a0;
    b0_ = b0u * invA0;
    b1_ = b1u * invA0;
    b2_ = b2u * invA0;
    a1_ = a1u * invA0;
    a2_ = a2u * invA0;

    dirty_ = false;
}

void BiquadFilterEffect::Process(float* output, uint32_t frames, uint32_t channels,
                                  const float* /*sidechain*/, uint32_t /*sidechainChannels*/) noexcept {
    if (channels == 0 || frames == 0) return;
    if (dirty_) RecomputeCoefficients();

    const uint32_t ch = std::min<uint32_t>(channels, kMaxChannels);

    // Direct-form II transposed: y = b0*x + z1; z1 = b1*x + z2 - a1*y; z2 = b2*x - a2*y
    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < ch; ++c) {
            const uint32_t i = f * channels + c;
            const float x    = output[i];
            const float y    = b0_ * x + z1_[c];
            z1_[c]           = b1_ * x + z2_[c] - a1_ * y;
            z2_[c]           = b2_ * x - a2_ * y;
            output[i]        = y;
        }
    }
}

void BiquadFilterEffect::OnParameter(uint16_t paramId, float value) noexcept {
    if (paramId == EffectParameter::Biquad_CutoffHz) {
        cutoffHz_ = value;
        dirty_    = true;
    } else if (paramId == EffectParameter::Biquad_Q) {
        Q_     = value;
        dirty_ = true;
    }
}

} // namespace audio
