// audio_engine/dsp/reverb_effect.cpp

#include "audio_engine/dsp/reverb_effect.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

// Scale Freeverb's 44.1 kHz delay lengths to the runtime sample rate.
inline uint32_t ScaleDelay(uint32_t base44k, uint32_t sampleRate) noexcept {
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(base44k) * sampleRate + 22050u) / 44100u);
}

inline float DbToLinear(float db) noexcept {
    return std::pow(10.0f, db / 20.0f);
}

} // namespace

ReverbEffect::ReverbEffect(float roomSize, float damping, float wetGainDb)
    : roomSize_(std::clamp(roomSize, 0.0f, 1.0f)),
      damping_(std::clamp(damping, 0.0f, 1.0f)),
      wetGainDb_(wetGainDb)
{
    // Freeverb's room-size and damping scaling.
    feedback_ = roomSize_ * 0.28f + 0.7f;       // ⇒ feedback in [0.70, 0.98]
    dampVal_  = damping_  * 0.4f;               // ⇒ damp in    [0.00, 0.40]
    RecomputeWetGain();
}

void ReverbEffect::RecomputeWetGain() noexcept {
    wetLin_ = DbToLinear(wetGainDb_);
}

void ReverbEffect::Prepare(uint32_t sampleRate, uint32_t channels) {
    sampleRate_ = (sampleRate == 0) ? 48000u : sampleRate;
    channels_   = (channels  == 0) ? 1u     : channels;

    const uint32_t spread = ScaleDelay(kStereoSpread, sampleRate_);
    for (uint32_t i = 0; i < kCombs; ++i) {
        const uint32_t lenL = ScaleDelay(kCombsLengths44[i], sampleRate_);
        const uint32_t lenR = lenL + spread;
        combL_[i].buf.assign(lenL, 0.0f);
        combR_[i].buf.assign(lenR, 0.0f);
        combL_[i].Reset();
        combR_[i].Reset();
    }
    for (uint32_t i = 0; i < kAllpass; ++i) {
        const uint32_t lenL = ScaleDelay(kAllpassLengths44[i], sampleRate_);
        const uint32_t lenR = lenL + spread;
        apL_[i].buf.assign(lenL, 0.0f);
        apR_[i].buf.assign(lenR, 0.0f);
        apL_[i].Reset();
        apR_[i].Reset();
    }
}

void ReverbEffect::Process(float* output, uint32_t frames, uint32_t channels,
                             const float* /*sidechain*/, uint32_t /*sidechainCh*/) noexcept {
    if (channels == 0 || frames == 0) return;

    // Freeverb's input gain; empirically chosen to keep the wet level
    // sensible relative to the dry signal at default room/damping.
    constexpr float kInputGain = 0.015f;

    for (uint32_t f = 0; f < frames; ++f) {
        // Read input from the bus's accumulated sample (interleaved).
        const float inL = output[f * channels + 0];
        const float inR = (channels >= 2)
            ? output[f * channels + 1]
            : inL;

        // Mix to mono (sum) for input to the comb network; this is what
        // Freeverb does; it broadens the stereo image via the L/R delay
        // offsets, not by feeding L and R independently.
        const float input = (inL + inR) * kInputGain;

        // 8 parallel combs per channel; sum the outputs.
        float sumL = 0.0f, sumR = 0.0f;
        for (uint32_t i = 0; i < kCombs; ++i) {
            sumL += combL_[i].Step(input, feedback_, dampVal_);
            sumR += combR_[i].Step(input, feedback_, dampVal_);
        }

        // 4 series allpass per channel.
        float yL = sumL, yR = sumR;
        for (uint32_t i = 0; i < kAllpass; ++i) {
            yL = apL_[i].Step(yL);
            yR = apR_[i].Step(yR);
        }

        // The reverb is a wet-only effect; overwrite the bus output with
        // the reverb tail. The dry signal lives on a different bus and
        // is summed at master.
        output[f * channels + 0] = yL * wetLin_;
        if (channels >= 2) output[f * channels + 1] = yR * wetLin_;
    }
}

void ReverbEffect::OnParameter(uint16_t paramId, float value) noexcept {
    switch (paramId) {
        case EffectParameter::Reverb_RoomSize:
            roomSize_ = std::clamp(value, 0.0f, 1.0f);
            feedback_ = roomSize_ * 0.28f + 0.7f;
            break;
        case EffectParameter::Reverb_Damping:
            damping_ = std::clamp(value, 0.0f, 1.0f);
            dampVal_ = damping_ * 0.4f;
            break;
        case EffectParameter::Reverb_WetGainDb:
            wetGainDb_ = value;
            RecomputeWetGain();
            break;
        default:
            break;
    }
}

// v0.28.0: introspection — mirror of OnParameter. Returns the stored
// target values; the derived feedback_/dampVal_/wetLinear_ scalars are
// recomputed from these on each OnParameter call so the raw targets
// are the authoritative read source.
float ReverbEffect::GetParameter(uint16_t paramId) const noexcept {
    switch (paramId) {
        case EffectParameter::Reverb_RoomSize:  return roomSize_;
        case EffectParameter::Reverb_Damping:   return damping_;
        case EffectParameter::Reverb_WetGainDb: return wetGainDb_;
        default:                                return 0.0f;
    }
}

} // namespace audio
