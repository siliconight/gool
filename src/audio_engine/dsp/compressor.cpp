// audio_engine/dsp/compressor.cpp

#include "audio_engine/dsp/compressor.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

constexpr float kMinDb = -120.0f;

float LinearToDb(float v) noexcept {
    return v > 1e-10f ? 20.0f * std::log10(v) : kMinDb;
}

float DbToLinear(float dB) noexcept {
    return std::pow(10.0f, dB * 0.05f);
}

// Standard one-pole exponential smoothing coefficient for a given time
// constant in milliseconds at a given sample rate. The output state reaches
// 1 - 1/e (~63%) of a step input after `tauMs` milliseconds.
float TimeConstantCoeff(float tauMs, uint32_t sampleRate) noexcept {
    const float tauSamples = std::max(1.0f, tauMs * 0.001f * static_cast<float>(sampleRate));
    return std::exp(-1.0f / tauSamples);
}

} // namespace

CompressorEffect::CompressorEffect(float thresholdDb,
                                    float ratio,
                                    float attackMs,
                                    float releaseMs,
                                    float makeupDb,
                                    BusId sidechainBus)
    : thresholdDb_(thresholdDb),
      ratio_(std::max(1.0f, ratio)),
      attackMs_(attackMs),
      releaseMs_(releaseMs),
      makeupDb_(makeupDb),
      sidechainBus_(sidechainBus) {}

void CompressorEffect::Prepare(uint32_t sampleRate, uint32_t /*channels*/) {
    sampleRate_  = sampleRate > 0 ? sampleRate : 48000;
    coeffDirty_  = true;
    envelopeDb_  = kMinDb;
    lastReductionDb_ = 0.0f;
}

void CompressorEffect::Process(float* output, uint32_t frames, uint32_t channels,
                                const float* sidechain, uint32_t sidechainChannels) noexcept {
    if (channels == 0 || frames == 0) return;

    if (coeffDirty_) {
        attackCoeff_  = TimeConstantCoeff(attackMs_,  sampleRate_);
        releaseCoeff_ = TimeConstantCoeff(releaseMs_, sampleRate_);
        coeffDirty_   = false;
    }

    // Decide which buffer drives the envelope:
    // * if sidechain is non-null, use it (true sidechain mode);
    // * otherwise self-sidechain on `output`.
    const float*   detect       = (sidechain != nullptr) ? sidechain : output;
    const uint32_t detectChans  = (sidechain != nullptr) ? sidechainChannels : channels;
    if (detectChans == 0) return;

    const float makeupLin = DbToLinear(makeupDb_);
    const float ratioInv  = 1.0f / ratio_;
    float       envDb     = envelopeDb_;
    float       lastRed   = 0.0f;

    for (uint32_t f = 0; f < frames; ++f) {
        // Peak across detect channels for this frame.
        float peak = 0.0f;
        for (uint32_t c = 0; c < detectChans; ++c) {
            const float s = std::abs(detect[f * detectChans + c]);
            if (s > peak) peak = s;
        }
        const float peakDb = LinearToDb(peak);

        // One-pole envelope: attack on rising signal, release on falling.
        const float coeff = (peakDb > envDb) ? attackCoeff_ : releaseCoeff_;
        envDb = peakDb + coeff * (envDb - peakDb);

        // Static hard-knee gain computer.
        float reductionDb = 0.0f;
        if (envDb > thresholdDb_) {
            // Above threshold: signal compressed by (over-threshold)*(1 - 1/ratio).
            reductionDb = (envDb - thresholdDb_) * (1.0f - ratioInv);
        }
        const float gainLin = DbToLinear(-reductionDb) * makeupLin;
        lastRed = reductionDb;

        // Apply uniform gain across all channels of the main output.
        for (uint32_t c = 0; c < channels; ++c) {
            output[f * channels + c] *= gainLin;
        }
    }

    envelopeDb_      = envDb;
    lastReductionDb_ = lastRed;
}

void CompressorEffect::OnParameter(uint16_t paramId, float value) noexcept {
    switch (paramId) {
        case EffectParameter::Compressor_ThresholdDb: thresholdDb_ = value; break;
        case EffectParameter::Compressor_Ratio:       ratio_       = std::max(1.0f, value); break;
        case EffectParameter::Compressor_AttackMs:    attackMs_    = std::max(0.1f, value); coeffDirty_ = true; break;
        case EffectParameter::Compressor_ReleaseMs:   releaseMs_   = std::max(0.1f, value); coeffDirty_ = true; break;
        case EffectParameter::Compressor_MakeupDb:    makeupDb_    = value; break;
        default: break;
    }
}

} // namespace audio
