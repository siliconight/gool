// audio_engine/dsp/compressor.cpp
//
// See compressor.h for the topology overview. This file implements:
//
//   * Peak / RMS envelope detection. Peak uses an asymmetric one-pole
//     follower with separate attack / release time constants (and an
//     optional hold timer that pauses release after each attack frame).
//     RMS uses a symmetric one-pole low-pass on squared samples — true
//     mean-square averaging — with the release coefficient as the
//     averaging time constant. Attack and hold are no-ops in RMS mode
//     because averaging is symmetric by nature.
//   * Sidechain HPF — inline mono biquad on the detection signal so
//     low-frequency content doesn't over-trigger the compressor.
//   * Soft- or hard-knee gain computer (Reiss/McPherson formula).
//   * Range cap on the resulting reduction.
//   * Makeup gain + dry/wet mix as a final algebraic combine
//     (single multiply per sample — no scratch buffer needed).

#include "audio_engine/dsp/compressor.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

constexpr float kMinDb       = -120.0f;
constexpr float kPi          = 3.14159265358979323846f;
constexpr float kSqrtHalf    = 0.70710678118654752440f;  // 1/sqrt(2)

// Note: LinearToDb is inlined where used because the multiplier
// differs by detection mode (10 for RMS mean-square, 20 for peak
// amplitude). A single helper would have to know the mode.

float DbToLinear(float dB) noexcept {
    return std::pow(10.0f, dB * 0.05f);
}

// Standard one-pole exponential smoothing coefficient for a given
// time constant in milliseconds at a given sample rate. The output
// reaches 1 - 1/e (~63%) of a step input after `tauMs` milliseconds.
float TimeConstantCoeff(float tauMs, uint32_t sampleRate) noexcept {
    const float tauSamples =
        std::max(1.0f, tauMs * 0.001f * static_cast<float>(sampleRate));
    return std::exp(-1.0f / tauSamples);
}

// Soft-knee gain reduction in dB.
//   x = current envelope (dB)
//   T = threshold (dB)
//   W = knee width (dB)        — 0 means hard knee
//   ratioInv = 1 / ratio
//
// Reduction is the amount BELOW the input the compressed output sits.
// Returns 0 below the knee, ramps quadratically through the knee, and
// returns the standard linear (x - T)*(1 - 1/R) above it.
float ComputeReductionDb(float x, float T, float W, float ratioInv) noexcept {
    if (W <= 0.0f) {
        // Hard knee: original v0.7 math.
        return (x > T) ? (x - T) * (1.0f - ratioInv) : 0.0f;
    }
    const float halfW = 0.5f * W;
    if (x <= T - halfW) {
        // Below knee: no compression.
        return 0.0f;
    }
    if (x >= T + halfW) {
        // Above knee: full compression.
        return (x - T) * (1.0f - ratioInv);
    }
    // In knee: quadratic interpolation. The output curve is
    //     y = x + (1/R - 1) * (x - T + W/2)^2 / (2 W)
    // so reduction = x - y = (1 - 1/R) * (x - T + W/2)^2 / (2 W).
    const float over = x - T + halfW;
    return (1.0f - ratioInv) * over * over / (2.0f * W);
}

// RBJ-cookbook high-pass biquad coefficients. Q = 1/sqrt(2)
// (Butterworth) gives the textbook 12 dB/oct rolloff with no
// resonance. Coefficients are direct-form-II-transposed so they
// match the storage shape the compressor uses.
void DesignHpf(float cutoffHz, uint32_t sampleRate,
                float& b0, float& b1, float& b2,
                float& a1, float& a2) noexcept {
    const float clamped = std::clamp(cutoffHz,
        20.0f, 0.49f * static_cast<float>(sampleRate));
    const float w0 = 2.0f * kPi * clamped / static_cast<float>(sampleRate);
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float Q = kSqrtHalf;
    const float alpha = sinw0 / (2.0f * Q);

    const float a0 = 1.0f + alpha;
    const float invA0 = 1.0f / a0;
    b0 =  ((1.0f + cosw0) * 0.5f) * invA0;
    b1 = -(1.0f + cosw0)          * invA0;
    b2 =  ((1.0f + cosw0) * 0.5f) * invA0;
    a1 =  (-2.0f * cosw0)         * invA0;
    a2 =  (1.0f - alpha)          * invA0;
}

// Single-sample biquad step (DF-II-T).
inline float BiquadStep(float in,
                          float b0, float b1, float b2,
                          float a1, float a2,
                          float& z1, float& z2) noexcept {
    const float out = b0 * in + z1;
    z1 = b1 * in - a1 * out + z2;
    z2 = b2 * in - a2 * out;
    return out;
}

} // namespace

CompressorEffect::CompressorEffect(const CompressorConfig& cfg)
    : thresholdDb_(cfg.thresholdDb),
      ratio_(std::max(1.0f, cfg.ratio)),
      attackMs_(std::max(0.1f, cfg.attackMs)),
      releaseMs_(std::max(0.1f, cfg.releaseMs)),
      makeupDb_(cfg.makeupDb),
      sidechainBus_(cfg.sidechainBus),
      kneeWidthDb_(std::max(0.0f, cfg.kneeWidthDb)),
      mixRatio_(std::clamp(cfg.mixRatio, 0.0f, 1.0f)),
      maxReductionDb_(std::max(0.0f, cfg.maxReductionDb)),
      sidechainHpfHz_(std::max(0.0f, cfg.sidechainHpfHz)),
      holdMs_(std::max(0.0f, cfg.holdMs)),
      detectionMode_(cfg.detectionMode) {}

void CompressorEffect::Prepare(uint32_t sampleRate, uint32_t /*channels*/) {
    sampleRate_      = sampleRate > 0 ? sampleRate : 48000;
    coeffDirty_      = true;
    envelopeDb_      = kMinDb;
    lastReductionDb_ = 0.0f;
    holdSamplesRemaining_ = 0;
    // Reset sidechain biquad state.
    scZ1_ = 0.0f;
    scZ2_ = 0.0f;
}

void CompressorEffect::RecomputeCoeffs() noexcept {
    attackCoeff_  = TimeConstantCoeff(attackMs_,  sampleRate_);
    releaseCoeff_ = TimeConstantCoeff(releaseMs_, sampleRate_);
    holdSamples_  = static_cast<uint32_t>(std::lround(
        holdMs_ * 0.001f * static_cast<float>(sampleRate_)));
    if (sidechainHpfHz_ > 0.0f) {
        DesignHpf(sidechainHpfHz_, sampleRate_,
                   scB0_, scB1_, scB2_, scA1_, scA2_);
    }
    coeffDirty_ = false;
}

void CompressorEffect::Process(float* output, uint32_t frames, uint32_t channels,
                                const float* sidechain, uint32_t sidechainChannels) noexcept {
    if (channels == 0 || frames == 0) return;

    if (coeffDirty_) RecomputeCoeffs();

    // Decide which buffer drives the envelope:
    //   * if sidechain is non-null, use it (true sidechain mode);
    //   * otherwise self-sidechain on `output`.
    const float*   detect      = (sidechain != nullptr) ? sidechain : output;
    const uint32_t detectChans = (sidechain != nullptr) ? sidechainChannels : channels;
    if (detectChans == 0) return;

    const float makeupLin = DbToLinear(makeupDb_);
    const float ratioInv  = 1.0f / ratio_;
    const bool  hpfOn     = (sidechainHpfHz_ > 0.0f);
    const bool  rmsMode   = (detectionMode_ == DetectionMode::Rms);

    // Envelope is stored in dB across calls (envelopeDb_) to keep the
    // public CurrentEnvelopeDb() diagnostic stable. Internally we
    // run the smoother in *linear* space because RMS mode needs to
    // smooth squared-magnitude (mean-square) cleanly. We seed envLin
    // from envelopeDb_ on entry and write back the dB equivalent on
    // exit.
    //
    // Linear-space semantics:
    //   Peak: envLin = |sample|       smoothed
    //   RMS:  envLin = sample^2       smoothed (mean-square)
    // The gain computer wants dB; the conversion factor differs
    // because amplitude→dB is 20*log10, while squared→dB is
    // 10*log10.
    float envLin = rmsMode
        ? std::pow(10.0f, envelopeDb_ * 0.1f)   // dB → squared-linear
        : std::pow(10.0f, envelopeDb_ * 0.05f); // dB → amplitude
    float lastRed = 0.0f;
    uint32_t holdRemaining = holdSamplesRemaining_;

    for (uint32_t f = 0; f < frames; ++f) {
        // ---- Detection: sum to mono, optional HPF ----
        float monoSum = 0.0f;
        for (uint32_t c = 0; c < detectChans; ++c) {
            monoSum += detect[f * detectChans + c];
        }
        const float monoAvg = monoSum / static_cast<float>(detectChans);

        const float filtered = hpfOn
            ? BiquadStep(monoAvg, scB0_, scB1_, scB2_, scA1_, scA2_, scZ1_, scZ2_)
            : monoAvg;

        // Per-frame detection magnitude in linear space:
        //   Peak: |sample|         (amplitude)
        //   RMS:  sample^2         (squared, contributes to mean-square)
        const float absF      = std::abs(filtered);
        const float detectLin = rmsMode ? (absF * absF) : absF;

        // ---- Attack / Release / Hold (in linear space) ----
        // Behavior depends on detection mode:
        //   Peak: asymmetric one-pole follower — attack rises fast,
        //         release falls slow, with optional hold that pauses
        //         release for a configured duration after each attack
        //         frame. Standard transient-tracking compressor topology.
        //   RMS:  symmetric one-pole low-pass on squared samples —
        //         the release coefficient is used as the averaging time
        //         constant. Attack and hold are no-ops in this mode
        //         because averaging is symmetric by nature: a true
        //         mean-square detector has one time constant, not two.
        //         Hosts who want to shape the post-RMS dynamics should
        //         compose downstream (e.g. layer two compressors).
        if (rmsMode) {
            envLin = detectLin + releaseCoeff_ * (envLin - detectLin);
        } else if (detectLin > envLin) {
            // Peak attack stage: env tracks rising signal.
            envLin = detectLin + attackCoeff_ * (envLin - detectLin);
            holdRemaining = holdSamples_;  // recharge on every attack frame
        } else {
            if (holdRemaining > 0) {
                --holdRemaining;
                // Env stays put — hold prevents premature release.
            } else {
                envLin = detectLin + releaseCoeff_ * (envLin - detectLin);
            }
        }

        // ---- Convert to dB for the gain computer ----
        // Peak: amplitude → dB = 20*log10(env). RMS: mean-square → dB
        // = 10*log10(env). Same dB output semantics either way: a
        // signal at -20 dBFS reads as -20 dB regardless of mode.
        const float envForCalcLin = std::max(envLin, 1e-20f);
        const float envDb = rmsMode
            ? 10.0f * std::log10(envForCalcLin)
            : 20.0f * std::log10(envForCalcLin);

        // ---- Gain computer ----
        float reductionDb =
            ComputeReductionDb(envDb, thresholdDb_, kneeWidthDb_, ratioInv);

        // ---- Range cap ----
        if (reductionDb > maxReductionDb_) reductionDb = maxReductionDb_;
        lastRed = reductionDb;

        // ---- Combine reduction + makeup + dry/wet into one multiply ----
        // wet = dry * gainLin where gainLin = 10^(-reductionDb/20) * makeupLin
        // mixed = mix * wet + (1 - mix) * dry
        //       = dry * (mix * gainLin + (1 - mix))
        const float gainLin = DbToLinear(-reductionDb) * makeupLin;
        const float effGain = mixRatio_ * gainLin + (1.0f - mixRatio_);

        for (uint32_t c = 0; c < channels; ++c) {
            output[f * channels + c] *= effGain;
        }
    }

    // Persist envelope state. Convert linear → dB once at exit so
    // the diagnostic accessor (CurrentEnvelopeDb) stays consistent
    // and the next Process() call resumes from the right value.
    {
        const float envForExitLin = std::max(envLin, 1e-20f);
        envelopeDb_ = rmsMode
            ? 10.0f * std::log10(envForExitLin)
            : 20.0f * std::log10(envForExitLin);
    }
    lastReductionDb_      = lastRed;
    holdSamplesRemaining_ = holdRemaining;
}

void CompressorEffect::OnParameter(uint16_t paramId, float value) noexcept {
    switch (paramId) {
        case EffectParameter::Compressor_ThresholdDb:
            thresholdDb_ = value;
            break;
        case EffectParameter::Compressor_Ratio:
            ratio_ = std::max(1.0f, value);
            break;
        case EffectParameter::Compressor_AttackMs:
            attackMs_ = std::max(0.1f, value);
            coeffDirty_ = true;
            break;
        case EffectParameter::Compressor_ReleaseMs:
            releaseMs_ = std::max(0.1f, value);
            coeffDirty_ = true;
            break;
        case EffectParameter::Compressor_MakeupDb:
            makeupDb_ = value;
            break;

        // ---- Tier A (v0.8) ----
        case EffectParameter::Compressor_KneeWidthDb:
            kneeWidthDb_ = std::max(0.0f, value);
            break;
        case EffectParameter::Compressor_MixRatio:
            mixRatio_ = std::clamp(value, 0.0f, 1.0f);
            break;
        case EffectParameter::Compressor_MaxReductionDb:
            maxReductionDb_ = std::max(0.0f, value);
            break;
        case EffectParameter::Compressor_SidechainHpfHz:
            sidechainHpfHz_ = std::max(0.0f, value);
            // Reset filter state when crossing the engaged/bypass
            // threshold so we don't carry over zero-state into a
            // newly-engaged filter (avoids initial click).
            scZ1_ = 0.0f;
            scZ2_ = 0.0f;
            coeffDirty_ = true;
            break;
        case EffectParameter::Compressor_HoldMs:
            holdMs_ = std::max(0.0f, value);
            coeffDirty_ = true;
            break;
        case EffectParameter::Compressor_DetectionMode:
            // 0.0 = Peak, anything else = Rms (matches the runtime ID
            // doc in bus.h's EffectParameter namespace).
            detectionMode_ = (value < 0.5f)
                ? DetectionMode::Peak
                : DetectionMode::Rms;
            break;
        default:
            break;
    }
}

// v0.28.0: introspection — mirror of OnParameter. Each case returns
// the target value stored from the most recent OnParameter call (or
// the value from the initial CompressorConfig). DetectionMode is
// returned as 0.0 = Peak, 1.0 = Rms, matching the encoding used by
// OnParameter.
float CompressorEffect::GetParameter(uint16_t paramId) const noexcept {
    switch (paramId) {
        case EffectParameter::Compressor_ThresholdDb:    return thresholdDb_;
        case EffectParameter::Compressor_Ratio:          return ratio_;
        case EffectParameter::Compressor_AttackMs:       return attackMs_;
        case EffectParameter::Compressor_ReleaseMs:      return releaseMs_;
        case EffectParameter::Compressor_MakeupDb:       return makeupDb_;
        case EffectParameter::Compressor_KneeWidthDb:    return kneeWidthDb_;
        case EffectParameter::Compressor_MixRatio:       return mixRatio_;
        case EffectParameter::Compressor_MaxReductionDb: return maxReductionDb_;
        case EffectParameter::Compressor_SidechainHpfHz: return sidechainHpfHz_;
        case EffectParameter::Compressor_HoldMs:         return holdMs_;
        case EffectParameter::Compressor_DetectionMode:
            return (detectionMode_ == DetectionMode::Rms) ? 1.0f : 0.0f;
        default:
            return 0.0f;
    }
}

} // namespace audio
