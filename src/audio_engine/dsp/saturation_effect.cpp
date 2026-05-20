// audio_engine/dsp/saturation_effect.cpp
//
// See saturation_effect.h for topology overview and the design doc
// at docs/audio_design/saturation_v2.md for the ADAA derivation.

#include "audio_engine/dsp/saturation_effect.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

// Numerically stable log(cosh(x)).
//
// Direct evaluation as std::log(std::cosh(x)) overflows for |x| > ~700
// (since cosh grows exponentially). The identity used here is exact:
//
//   log(cosh(x)) = |x| + log((1 + exp(-2|x|)) / 2)
//                = |x| + log1p(exp(-2|x|)) - log(2)
//
// For very large |x|, exp(-2|x|) underflows to zero and the result
// asymptotes correctly to |x| - log(2). For x = 0, log1p(1) - log(2)
// = 0 (matching log(cosh(0)) = log(1) = 0). Stable across the full
// double range of interest.
inline double log_cosh(double x) noexcept {
    const double ax = std::abs(x);
    return ax + std::log1p(std::exp(-2.0 * ax)) - 0.6931471805599453; // log(2)
}

// First-order ADAA for f(x) = tanh(x). Returns the antialiased value
// y[n] given the current driven sample x and the previous driven
// sample x_prev. The midpoint fallback handles the ill-conditioned
// case where the denominator (x - x_prev) approaches zero, which
// happens at steady-state DC and during quiet passages where adjacent
// samples are nearly equal. The threshold ε = 1e-6 is chosen for
// double-precision arithmetic; at single precision it would need to
// be ~1e-3 and would noticeably miss aliasing reduction on slowly
// varying signals.
inline double adaa1_tanh(double x, double x_prev) noexcept {
    const double diff = x - x_prev;
    constexpr double kEps = 1.0e-6;
    if (std::abs(diff) < kEps) {
        // Midpoint fallback — same result as the trivial shaper at
        // the geometric mean of the two endpoints.
        return std::tanh(0.5 * (x + x_prev));
    }
    return (log_cosh(x) - log_cosh(x_prev)) / diff;
}

} // namespace

SaturationEffect::SaturationEffect(const SaturationConfig& cfg)
    : drive_(std::max(0.0f, cfg.drive)),
      mix_(std::clamp(cfg.mix, 0.0f, 1.0f)),
      outputGain_(std::max(0.0f, cfg.outputGain)),
      bias_(std::clamp(cfg.bias, -1.0f, 1.0f)) {}

void SaturationEffect::Prepare(uint32_t /*sampleRate*/, uint32_t channels) {
    // v0.38.0: ADAA requires per-channel state for x[n-1]. Size to the
    // current channel count and zero-init — a cold start treats every
    // channel as if the previous sample was silence. Sample rate is
    // unused (ADAA is sample-rate-independent; the antiderivative
    // takes care of the bandlimiting).
    prevDriven_.assign(channels, 0.0);
}

void SaturationEffect::Process(float* output, uint32_t frames, uint32_t channels,
                                 const float* /*sidechain*/, uint32_t /*sidechainChannels*/) noexcept {
    if (channels == 0 || frames == 0) return;

    // Bypass when fully dry — saves the per-sample tanh on every bus
    // that has the effect installed but turned off (the default
    // SaturationConfig leaves mix at 0). This is the "free if unused"
    // case; we want it cheap because hosts will leave the effect
    // installed and modulate mix from gameplay code.
    //
    // Note: prevDriven_ is intentionally NOT updated during bypass.
    // A mix=0 → mix>0 transition therefore uses stale state for one
    // sample. The resulting transient is small (the immediate ADAA
    // output blends with whatever the previous driven value was) and
    // gets further attenuated by the mix coefficient, which is itself
    // typically small at the start of an automated ramp.
    if (mix_ <= 0.0f) return;

    // Defensive: if Prepare() wasn't called or the channel count
    // changed since, fall back to a stateless midpoint evaluation by
    // resizing here. Real hosts will have called Prepare() with the
    // correct channel count, but tests that bypass Prepare or
    // hot-plug channel counts shouldn't crash.
    if (prevDriven_.size() != channels) {
        prevDriven_.assign(channels, 0.0);
    }

    // Snapshot parameter values to locals — OnParameter writes to the
    // underscore-suffixed members, but Process reads the snapshot. Two
    // benefits: (a) one parameter mid-buffer change can't half-process
    // a frame with stale drive and fresh mix, (b) the compiler can
    // hoist the constants out of the inner loop.
    const double drive    = static_cast<double>(drive_);
    const double bias     = static_cast<double>(bias_);
    const float  mix      = mix_;
    const float  outG     = outputGain_;
    const float  invMix   = 1.0f - mix;

    // DC removal: when bias != 0, the steady-state output of the
    // shaper for any constant input includes a contribution from
    // tanh(bias * drive). Subtract that constant from the wet path
    // so silent input produces silent output regardless of bias
    // setting. (Same logic as pre-v0.38.0; the ADAA shaper has the
    // same steady-state DC behavior as trivial tanh, so this carries
    // over unchanged.)
    const double dcRemove = (bias != 0.0) ? std::tanh(bias * drive) : 0.0;

    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < channels; ++c) {
            const uint32_t idx = f * channels + c;
            const float  dry    = output[idx];
            const double driven = (static_cast<double>(dry) + bias) * drive;
            const double yAdaa  = adaa1_tanh(driven, prevDriven_[c]);
            const float  wet    = static_cast<float>((yAdaa - dcRemove) * static_cast<double>(outG));
            output[idx]         = dry * invMix + wet * mix;
            prevDriven_[c]      = driven;
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
