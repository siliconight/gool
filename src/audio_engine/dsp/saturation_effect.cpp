// audio_engine/dsp/saturation_effect.cpp
//
// See saturation_effect.h for topology overview and the design doc
// at docs/audio_design/saturation_v2.md for the ADAA derivation and
// per-mode rationale.

#include "audio_engine/dsp/saturation_effect.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

// ---- Shape functions f_m(x) and first antiderivatives F_m(x) ------
//
// Each mode is a pair: the memoryless nonlinearity used for the
// trivial (non-ADAA) path and the midpoint fallback, plus its
// closed-form first antiderivative used for the ADAA divide. All
// computed in double precision to keep the (F(x) - F(x_prev)) /
// (x - x_prev) divide well-conditioned near the ε threshold.

// Tanh mode (mode 0). Symmetric, smooth shoulder, odd-harmonic
// dominant. Pre-v0.40.0's only shape. The log_cosh form is
// numerically stable for large |x| via the identity
//   log(cosh(x)) = |x| + log1p(exp(-2|x|)) - log(2)
// Direct std::log(std::cosh(x)) overflows for |x| > ~700; this
// asymptotes correctly to |x| - log(2) for large |x|.
inline double f_tanh(double x) noexcept {
    return std::tanh(x);
}
inline double F_tanh(double x) noexcept {
    const double ax = std::abs(x);
    return ax + std::log1p(std::exp(-2.0 * ax)) - 0.6931471805599453;  // log(2)
}

// Tube mode (mode 1). asinh(x) normalized so the slope at zero is
// 1·(1/asinh(1)) ≈ 1.135 — slightly steeper than tanh at zero, which
// gives a more open / "transparent" character at low drive while
// preserving asinh's gentle unbounded shoulder. The normalization
// keeps the per-mode useful drive range (1..3) acoustically
// comparable to Tanh's (1..4) at matching norm-drive positions.
inline double f_tube(double x) noexcept {
    // 1.0 / asinh(1.0) = 1.0 / 0.8813735870195430 = 1.13461506864858788...
    constexpr double kInvAsinh1 = 1.1346150686485879;
    return std::asinh(x) * kInvAsinh1;
}
inline double F_tube(double x) noexcept {
    // ∫asinh(x) dx = x·asinh(x) - sqrt(1 + x²). Multiply by the
    // normalization constant 1/asinh(1) to match f_tube's slope.
    constexpr double kInvAsinh1 = 1.1346150686485879;
    return (x * std::asinh(x) - std::sqrt(1.0 + x * x)) * kInvAsinh1;
}

// Tape mode (mode 2). Soft quadratic clipping per Zölzer:
//   f(x) = sign(x) · (1 - (1 - |x|)²)  for |x| ≤ 1
//        = sign(x)                       for |x| > 1
// Expanding (1 - |x|)² = 1 - 2|x| + x² gives the equivalent form
//   f(x) = 2x - sign(x)·x²              for |x| ≤ 1
// which is cheaper to compute (one branch on sign, one multiply
// rather than two subtractions and a square). Bounded ±1 with a
// parabolic shoulder — gentle dynamics compression baked into the
// shape, distinct from Tanh's softer-bend asymptote.
inline double f_tape(double x) noexcept {
    const double ax = std::abs(x);
    if (ax >= 1.0) return (x >= 0.0) ? 1.0 : -1.0;
    return 2.0 * x - std::copysign(x * x, x);
}
inline double F_tape(double x) noexcept {
    // Inside region |x| ≤ 1: f(x) = 2x - sign(x)·x², so the
    // antiderivative is x² - |x|³/3. The |x|³ form is correct because
    //   d/dx[x²] = 2x
    //   d/dx[|x|³ / 3] = x·|x|       (since d/dx|x|³ = 3·x·|x|)
    // and 2x - x·|x| = f(x) exactly:
    //   x>0: 2x - x·x = 2x - x² ✓
    //   x<0: 2x - x·(-x) = 2x + x² ✓
    // The earlier draft used copysign(x³, x), which equals x³ for
    // every x (positive or negative) and produced a discontinuity of
    // 2/3 at x=-1 plus a wrong derivative for x<0. The |x|³ form
    // gives the canonical (even-symmetric) antiderivative which is
    // continuous at both ±1.
    // Outside region |x| > 1: ∫sign(x) dx = |x| + C, with C from
    // continuity at x=±1: inside = 1 - 1/3 = 2/3, outside = 1 + C,
    // so C = -1/3.
    const double ax = std::abs(x);
    if (ax >= 1.0) return ax - (1.0 / 3.0);
    return x * x - (ax * x * x) / 3.0;
}

// Diode mode (mode 3). Cubic clip per Pirkle Addendum A19:
//   f(x) = x - x³/3                     for |x| < 1
//        = sign(x) · (2/3)               for |x| ≥ 1
// Bounded ±2/3 with a much sharper shoulder than Tape — punchy,
// odd-dominant, well-suited to radio-comms / broken-speaker FX and
// gunshot bite. The shoulder hardness is exactly what makes
// first-order ADAA less effective on this mode (~28 dB aliasing
// reduction vs ~40 dB on Tanh); higher-order ADAA would close the
// gap but at compute cost we're not willing to pay.
inline double f_diode(double x) noexcept {
    const double ax = std::abs(x);
    if (ax >= 1.0) return (x >= 0.0) ? (2.0 / 3.0) : -(2.0 / 3.0);
    return x - x * x * x / 3.0;
}
inline double F_diode(double x) noexcept {
    // Inside region |x| < 1: ∫(x - x³/3) dx = x²/2 - x⁴/12
    // Outside region |x| ≥ 1: ∫sign(x)·(2/3) dx = (2/3)·|x| + C, with
    // C from continuity at x=1: inside form = 1/2 - 1/12 = 5/12;
    // outside form = 2/3 + C; equating gives C = 5/12 - 2/3 = -1/4.
    const double ax = std::abs(x);
    if (ax >= 1.0) return (2.0 / 3.0) * ax - 0.25;
    const double x2 = x * x;
    return x2 * 0.5 - x2 * x2 / 12.0;
}

// ---- Generic first-order ADAA wrapper -----------------------------
//
// Templated on the shape and antiderivative functions; gets
// inlined-and-monomorphized per mode at compile time, so the dispatch
// is a single switch on mode in Process() with no runtime function-
// pointer overhead per sample.
template <typename Shape, typename Antiderivative>
inline double adaa1(double x, double x_prev, Shape f, Antiderivative F) noexcept {
    const double diff = x - x_prev;
    constexpr double kEps = 1.0e-6;
    if (std::abs(diff) < kEps) {
        // Midpoint fallback — analytically equal to the trivial shape
        // evaluated at the average of the two endpoints. Used when
        // adjacent samples are too close for the divide to be stable
        // (steady-state DC, quiet passages, low-rate envelopes).
        return f(0.5 * (x + x_prev));
    }
    return (F(x) - F(x_prev)) / diff;
}

// ---- Per-mode drive normalization ---------------------------------
//
// Maps the normalized 0..1 drive parameter to a per-mode useful
// input scale on [1, N_mode], per saturation_v2.md §6.1. The +1
// baseline keeps norm=0 ≈ linear (no harmonics generated regardless
// of mode).
//
// Pre-v0.40.0 the only mode was Tanh and the drive was passed
// directly as the input scale. For round-trip JSON compatibility,
// MapNormDriveToScale(Tanh, normDrive) must equal the soft-migrated
// legacy drive value, i.e. for legacy drive D in [1..4],
//   normDrive = (D - 1) / 3
//   scale     = 1 + 3 · normDrive = D    ✓
// Legacy drives > 4 clamp to scale 4 (mild capping, but >4 was
// extreme territory anyway).
inline double MapNormDriveToScale(SaturationMode m, double normDrive) noexcept {
    const double n = std::clamp(normDrive, 0.0, 1.0);
    switch (m) {
        case SaturationMode::Tanh:  return 1.0 + 3.0 * n;   // 1..4
        case SaturationMode::Tube:  return 1.0 + 2.0 * n;   // 1..3
        case SaturationMode::Tape:  return 1.0 + 2.0 * n;   // 1..3
        case SaturationMode::Diode: return 1.0 + 5.0 * n;   // 1..6
    }
    return 1.0;
}

// Low-drive bypass crossfade per saturation_v2.md §7.3. Returns the
// blend coefficient α in [0..1]: 0 = pure trivial shape (no ADAA
// compute), 1 = pure ADAA. The 0.10..0.30 crossfade region prevents
// the ADAA-introduced low-amplitude noise from rising above the
// floor on very quiet signals, while still engaging full ADAA before
// drive levels that actually generate audible harmonics.
inline double ADAAMixCoeff(double normDrive) noexcept {
    if (normDrive <= 0.10) return 0.0;
    if (normDrive >= 0.30) return 1.0;
    return (normDrive - 0.10) * 5.0;   // (n - 0.10) / 0.20
}

// ---- Templated per-mode inner loop --------------------------------
//
// Instantiated once per mode by the switch in Process(). The body
// is identical across modes apart from the f / F template params;
// the compiler inlines those calls and the resulting four
// instantiations are roughly equal cost apart from the underlying
// shape evaluation (transcendental for Tanh/Tube, polynomial for
// Tape/Diode).
template <typename Shape, typename Antiderivative>
inline void ProcessOneMode(float* output, uint32_t frames, uint32_t channels,
                           std::vector<double>& prevDriven,
                           double driveScale, double bias,
                           float  mix, float invMix, float outG,
                           double adaaCoeff,
                           Shape f, Antiderivative F) noexcept {
    // DC removal: when bias != 0 the shaper has a constant DC term at
    // its output equal to f(bias · driveScale). Subtract that before
    // the dry/wet mix so silent input always produces silent output
    // regardless of bias setting.
    const double dcRemove = (bias != 0.0) ? f(bias * driveScale) : 0.0;
    const double oneMinusAdaa = 1.0 - adaaCoeff;

    for (uint32_t fr = 0; fr < frames; ++fr) {
        for (uint32_t c = 0; c < channels; ++c) {
            const uint32_t idx    = fr * channels + c;
            const float    dry    = output[idx];
            const double   driven = (static_cast<double>(dry) + bias) * driveScale;

            // Always compute the trivial shape (cheap; needed for the
            // crossfade). Compute ADAA only when α > 0 — saves the
            // antiderivative work in the low-drive bypass region.
            const double yTrivial = f(driven);
            double yShaped = yTrivial;
            if (adaaCoeff > 0.0) {
                const double yAdaa = adaa1(driven, prevDriven[c], f, F);
                yShaped = oneMinusAdaa * yTrivial + adaaCoeff * yAdaa;
            }

            const float wet = static_cast<float>(
                (yShaped - dcRemove) * static_cast<double>(outG));
            output[idx]    = dry * invMix + wet * mix;
            prevDriven[c]  = driven;
        }
    }
}

}  // namespace

SaturationEffect::SaturationEffect(const SaturationConfig& cfg)
    : drive_(std::clamp(cfg.drive, 0.0f, 1.0f)),     // v0.40.0: normalized
      mix_(std::clamp(cfg.mix, 0.0f, 1.0f)),
      outputGain_(std::max(0.0f, cfg.outputGain)),
      bias_(std::clamp(cfg.bias, -1.0f, 1.0f)),
      mode_(cfg.mode) {}

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

    // Bypass when fully dry — saves all per-sample work on every bus
    // that has the effect installed but turned off (the default
    // SaturationConfig leaves mix at 0). Note: prevDriven_ is
    // intentionally NOT updated during bypass; a mix=0 → mix>0
    // transition therefore uses stale state for one sample.
    if (mix_ <= 0.0f) return;

    // Defensive: if Prepare() wasn't called or the channel count
    // changed since, resize state here. Tests that bypass Prepare or
    // hot-plug channel counts shouldn't crash.
    if (prevDriven_.size() != channels) {
        prevDriven_.assign(channels, 0.0);
    }

    // Snapshot parameters to locals. OnParameter writes to the
    // underscore-suffixed members; Process reads the snapshot so a
    // parameter change mid-buffer can't half-process a frame.
    const double         normDrive  = static_cast<double>(drive_);
    const SaturationMode mode       = mode_;
    const double         driveScale = MapNormDriveToScale(mode, normDrive);
    const double         bias       = static_cast<double>(bias_);
    const float          mix        = mix_;
    const float          outG       = outputGain_;
    const float          invMix     = 1.0f - mix;
    const double         adaaCoeff  = ADAAMixCoeff(normDrive);

    // Per-mode dispatch. Each branch instantiates the templated
    // inner loop with its mode's shape pair; the compiler inlines f
    // and F, so dispatch cost is one switch per buffer (negligible).
    switch (mode) {
        case SaturationMode::Tanh:
            ProcessOneMode(output, frames, channels, prevDriven_,
                            driveScale, bias, mix, invMix, outG, adaaCoeff,
                            f_tanh, F_tanh);
            break;
        case SaturationMode::Tube:
            ProcessOneMode(output, frames, channels, prevDriven_,
                            driveScale, bias, mix, invMix, outG, adaaCoeff,
                            f_tube, F_tube);
            break;
        case SaturationMode::Tape:
            ProcessOneMode(output, frames, channels, prevDriven_,
                            driveScale, bias, mix, invMix, outG, adaaCoeff,
                            f_tape, F_tape);
            break;
        case SaturationMode::Diode:
            ProcessOneMode(output, frames, channels, prevDriven_,
                            driveScale, bias, mix, invMix, outG, adaaCoeff,
                            f_diode, F_diode);
            break;
    }
}

void SaturationEffect::OnParameter(uint16_t paramId, float value) noexcept {
    switch (paramId) {
        case EffectParameter::Saturation_Drive:
            // v0.40.0: drive is normalized 0..1. Callers passing
            // legacy unnormalized values via the parameter API (as
            // opposed to JSON config, which has soft-migration) get
            // clamped to 1.0; the per-mode driveScale mapping will
            // saturate at the mode's maximum useful input gain.
            drive_ = std::clamp(value, 0.0f, 1.0f);
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
        case EffectParameter::Saturation_Mode: {
            // v0.40.0: mode is a discrete int packed into a float for
            // the SetEffectParameter API. Round and clamp into the
            // enum's valid range (0..3); anything else stays at the
            // current mode rather than picking an arbitrary default.
            const int raw = static_cast<int>(std::lround(value));
            if (raw >= 0 && raw <= 3) {
                mode_ = static_cast<SaturationMode>(raw);
            }
            break;
        }
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
        case EffectParameter::Saturation_Mode:
            return static_cast<float>(static_cast<int>(mode_));
        default:                                     return 0.0f;
    }
}

} // namespace audio
