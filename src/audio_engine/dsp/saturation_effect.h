// Copyright 2026 Brannen Graves
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing permissions
// and limitations under the License.

// audio_engine/dsp/saturation_effect.h
//
// Multi-mode waveshaper with first-order Antiderivative Anti-Aliasing
// (ADAA). Four character modes — Tanh (default, symmetric console
// glue), Tube (gentle asinh-shaped warmth), Tape (soft-quadratic
// Zölzer compression), Diode (cubic clip with bite) — each with its
// own per-mode useful drive range mapped from the normalized 0..1
// drive parameter. Suitable for subtle bus glue, impact reinforcement,
// dialogue warmth, or aggressive radio-comms / gunshot character.
//
//   Topology (per mode m):
//
//       driveScale = 1 + N_m · normDrive       (N_Tanh=3, N_Tube=2,
//                                                N_Tape=2, N_Diode=5)
//       x[n]       = (dry[n] + bias) · driveScale
//       f_m(x)     = mode-specific shape (tanh / asinh-norm / soft-quad / cubic-clamped)
//       F_m(x)     = first antiderivative of f_m
//
//                            F_m(x[n]) - F_m(x[n-1])
//       y_adaa[n]  =  ───────────────────────────────  (|Δx| ≥ ε)
//                              x[n] - x[n-1]
//
//       y_adaa[n]  =  f_m((x[n] + x[n-1]) / 2)          (|Δx| < ε)
//
//       y[n]       = α · y_adaa[n] + (1-α) · f_m(x[n])   (low-drive
//                                                         crossfade, α
//                                                         from §7.3)
//
//       wet        = (y[n] - f_m(bias · driveScale)) · outputGain
//       out        = dry · (1 - mix) + wet · mix
//
//   * `drive`        - normalized 0..1, mapped to per-mode useful
//                      range. 0 = essentially linear pass-through,
//                      1 = max useful drive for the selected mode.
//                      Legacy unnormalized values > 1.0 are detected
//                      and soft-migrated at config load
//                      (bus_config_loader.cpp).
//   * `mix`          - parallel dry/wet blend. 0 = bypass (default,
//                      makes adding the effect to a bus a no-op).
//                      Subtle glue lives in 0.10–0.30.
//   * `outputGain`   - post-shaper trim, applied AFTER the v0.58.0
//                      auto-compensation. With Phase 3 landed, this
//                      is now a pure post-trim that the user sets to
//                      taste; the auto-compensation table keeps the
//                      wet path at consistent loudness across drive
//                      sweeps so a fader move on `outputGain` only
//                      changes level, never character. Range 0..2.
//   * `bias`         - DC offset added before shaping. The DC term
//                      that this introduces at the shaper output is
//                      removed by a per-channel one-pole DC blocker
//                      (v0.58.0; replaces the v0.40.0 static
//                      subtraction). Range -1..1. Per-mode
//                      semantics for Diode TBD (see saturation_v2.md
//                      §13 open question); v0.40.0 treats it as
//                      pre-shaper DC offset for all modes,
//                      consistent with Tanh/Tube/Tape.
//   * `mode`         - SaturationMode enum (0..3, see bus.h). Default
//                      0 (Tanh), which makes existing config files
//                      sound identical to pre-v0.40.0 binaries.
//   * `tone`         - v0.59.0 Phase 4 tone tilt, range -1..+1, default
//                      0 (filter bypassed — fast path). Implements a
//                      pre-emphasis / de-emphasis high-shelf pair at
//                      ~1 kHz with gain = `tone · 6 dB`. The de-
//                      emphasis filter is the algebraic inverse of the
//                      pre-emphasis filter, so on dry-equivalent
//                      (un-shaped) material the net tonal balance is
//                      preserved within filter ringing. The interesting
//                      effect happens BETWEEN the two filters, where
//                      the shaper sees a tonally biased signal:
//                      `tone > 0` lets HF content drive the shaper
//                      harder (brighter saturation, more transient
//                      clipping); `tone < 0` lets lows drive the
//                      shaper harder (darker saturation, more body
//                      emphasis). See saturation_v2.md §9.
//
// Per-channel state (allocated in Prepare, zero-init):
//   * `prevDriven_[c]`     — ADAA's x[n-1] in driven-input domain (double)
//   * `dcBlockerY1_[c]`    — v0.58.0 DC-blocker output history (float)
//   * `dcBlockerX1_[c]`    — v0.58.0 DC-blocker input history (float)
//   * `toneLpPre_[c]`      — v0.59.0 pre-emphasis one-pole LP state (float)
//   * `toneLpPost_[c]`     — v0.59.0 de-emphasis one-pole LP state (float)
//
// Parameter smoothing (v0.58.0): drive, mix, and bias are smoothed
// from their target values toward `current` values once per buffer
// with a ~20 ms time constant, removing the zipper-noise on rapid
// automation that pre-v0.58.0 had. Smoothing is per-buffer rather
// than per-sample because each buffer is short enough (typical
// 256–512 samples at 48 kHz = ~5–10 ms) that per-buffer steps are
// imperceptible while saving work in the hot path.
//
// v0.59.0: tone is also smoothed via the same target/current
// mechanism. A tone-knob automation curve from -1 to +1 over a
// half second therefore steps once per buffer, not once per sample,
// keeping the shelf-coefficient recompute outside the inner loop.
//
// Anti-aliasing (v0.38.0 Phase 1, extended to all modes in v0.40.0):
//   First-order ADAA per Parker, Zavalishin, Le Bivic, DAFX 2016. At
//   drive ≥ 2.5 on transient-rich sources the non-bandlimited shapers
//   introduce foldover aliasing well above the noise floor; ADAA
//   reduces aliasing-band energy by 30+ dB across all four modes
//   (Tanh: ~40 dB, Tube: ~38 dB, Tape: ~32 dB at the corner, Diode:
//   ~28 dB — the harder shoulder is less amenable to first-order
//   ADAA). Higher-order ADAA would close the gap; not worth the
//   compute cost for our use case.
//
//   Low-drive crossfade (saturation_v2.md §7.3): at normalized
//   drive < 0.10 the trivial shaper is used (no aliasing risk worth
//   the ADAA cost), 0.10..0.30 linearly crossfades to pure ADAA, and
//   > 0.30 is pure ADAA. The crossfade prevents the noise floor
//   bump that pure ADAA can introduce on very quiet signals.
//
//   Cost: ~3× a raw shape evaluation per active sample inside the
//   pure-ADAA region. Bypass (mix == 0) is unchanged and remains
//   essentially free. Tape and Diode are the cheapest modes (no
//   transcendentals); Tanh and Tube are roughly equal cost
//   (log/log1p+exp vs asinh+sqrt).

#ifndef AUDIO_ENGINE_DSP_SATURATION_EFFECT_H
#define AUDIO_ENGINE_DSP_SATURATION_EFFECT_H

#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/bus.h"

#include <vector>

namespace audio {

struct SaturationConfig {
    // v0.40.0: `drive` is normalized 0..1 (mapped per-mode internally).
    // Legacy values > 1.0 are accepted at this struct level but get
    // soft-migrated at JSON load (bus_config_loader.cpp); direct C++
    // callers are responsible for passing a normalized value.
    float          drive      = 0.0f;
    float          mix        = 0.0f;       // default-off: adding to a bus is a no-op
    float          outputGain = 1.0f;
    float          bias       = 0.0f;
    SaturationMode mode       = SaturationMode::Tanh;   // v0.40.0
    // v0.59.0: Phase 4 tone tilt. -1..+1, default 0 (filter bypassed).
    // See saturation_effect.h class doc and saturation_v2.md §9.
    float          tone       = 0.0f;
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
    // v0.58.0: drive/mix/bias accessors return the TARGET value (what
    // was set via constructor or OnParameter), not the active smoothed
    // value. Process uses the smoothed value internally; readback
    // here gives users predictable "what I asked for" semantics
    // matching pre-v0.58.0 behavior.
    float          Drive()      const noexcept { return targetDrive_; }
    float          Mix()        const noexcept { return targetMix_; }
    float          OutputGain() const noexcept { return outputGain_; }
    float          Bias()       const noexcept { return targetBias_; }
    SaturationMode Mode()       const noexcept { return mode_; }
    // v0.59.0: tone tilt (-1..+1). Returns target like Drive/Mix/Bias.
    float          Tone()       const noexcept { return targetTone_; }

private:
    // drive_ is the NORMALIZED 0..1 value; the per-mode useful-range
    // mapping (driveScale = 1 + N_mode · drive_) happens inside Process
    // every buffer. Storing the normalized form means OnParameter
    // / GetParameter / API consumers see a consistent 0..1 surface.
    //
    // v0.58.0: drive_/mix_/bias_ are the SMOOTHED values used by
    // Process. targetDrive_/targetMix_/targetBias_ hold the most
    // recent value set via OnParameter or the constructor; once per
    // buffer, Process ramps current toward target with a ~20 ms
    // time constant. outputGain_ and mode_ are NOT smoothed (output
    // gain is post-comp and would just delay the obvious effect;
    // mode changes can't be smoothed meaningfully since the shape
    // function literally changes).
    float          drive_;
    float          mix_;
    float          outputGain_;
    float          bias_;
    SaturationMode mode_;

    float          targetDrive_;     // v0.58.0
    float          targetMix_;       // v0.58.0
    float          targetBias_;      // v0.58.0
    float          smoothCoef_;      // v0.58.0; ~1 - exp(-bufFrames/(SR·tau))

    // v0.59.0: Phase 4 tone tilt. tone_ is the smoothed active value
    // used by Process; targetTone_ is the most-recent target from
    // OnParameter/constructor. Smoothed alongside drive/mix/bias.
    // Range -1..+1.
    float          tone_;
    float          targetTone_;
    // v0.59.0: one-pole low-pass coefficient for the shelf-split
    // filters (1 - exp(-2π·fc/SR), fc = 1 kHz). Set in Prepare from
    // sample rate; constant once Prepare runs.
    float          toneLpCoef_ = 0.0f;

    // v0.38.0: per-channel ADAA state — the previous post-driveScale
    // sample value x[n-1] = (dry[n-1] + bias) · driveScale. Resized
    // in Prepare(); not touched when mix_ == 0 (the bypass path), so
    // a mix=0 → mix>0 transition may produce one sample of staleness
    // before steady-state ADAA. Held in double precision because the
    // (F(x) - F(x_prev)) / (x - x_prev) divide is ill-conditioned in
    // float near the midpoint-fallback threshold.
    //
    // v0.40.0: the same buffer is reused across mode switches. After
    // a mode change, the first sample uses the previous mode's
    // x[n-1] to compute ADAA, which is acceptable: prevDriven_ holds
    // a driven-input value (not a shape output), and the input value
    // is mode-independent. The first sample's ADAA computation
    // therefore produces a valid (if discontinuous) result. Mode
    // switching mid-buffer is undefined behavior anyway since
    // OnParameter is called from the control thread; in practice
    // mode changes always happen between buffers.
    std::vector<double> prevDriven_;

    // v0.58.0: per-channel one-pole DC blocker state. Replaces the
    // pre-v0.58.0 static f(bias·driveScale) subtraction, which went
    // stale during bias automation. Filter is y[n] = wet[n] -
    // wet[n-1] + R·y[n-1]; R is computed in Prepare from sample
    // rate to give a ~30 Hz HPF (~25 µs phase shift at midband,
    // imperceptible). State is zero-initialized; the DC blocker
    // settles in ~30 ms at 48 kHz which is below the noise floor
    // for any reasonable input.
    std::vector<float>  dcBlockerY1_;
    std::vector<float>  dcBlockerX1_;
    float               dcBlockerR_ = 0.995f;   // recomputed in Prepare

    // v0.59.0: per-channel one-pole LP state for the tone shelf
    // filters. The shelf is implemented as
    //   HS(x) = x + (A - 1) · (x - LP(x))
    // with one LP per stage. Two separate states: pre-emphasis LP
    // tracks the input signal; de-emphasis LP tracks the post-shaper
    // wet signal. Zero-init on Prepare; converges in ~30 ms at fc=1 kHz.
    std::vector<float>  toneLpPre_;
    std::vector<float>  toneLpPost_;
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_SATURATION_EFFECT_H
