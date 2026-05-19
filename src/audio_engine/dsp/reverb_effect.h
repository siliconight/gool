// audio_engine/dsp/reverb_effect.h
//
// Dattorro plate reverb. From-scratch C++20 implementation of the
// topology described in Dattorro, "Effect Design Part 1: Reverberator
// and Other Filters" (1997). See docs/audio_design/reverb_dattorro.md
// for the full design context.
//
// Topology:
//
//   mono input → predelay → input diffuser (4 series allpasses) → tank
//
//   Tank is two halves cross-coupled in a figure-8:
//
//     half A: modAP1 → delay1 → damping shelf → AP2 → delay2 → × decay → half B
//     half B: modAP3 → delay3 → damping shelf → AP4 → delay4 → × decay → half A
//
//   L/R outputs are weighted sums of 7 taps each, read from specific
//   positions inside delay1/delay2/delay3/delay4 and ap2/ap4. The tap
//   positions are Dattorro's published "magic numbers" — they're what
//   gives the algorithm its characteristic stereo image from mono input.
//
// Parameters (six total, all 0..1 unless noted):
//
//   predelay_ms  (0..200)  delay between dry signal and reverb onset.
//                          Strongest cue for room SIZE perception.
//   decay        (0..1)    tank feedback gain; longer decay ⇒ longer tail.
//   lf_damping   (0..1)    low-frequency absorption in the tank feedback.
//                          Higher ⇒ less bass in the tail.
//   hf_damping   (0..1)    high-frequency absorption in the tank feedback.
//                          Higher ⇒ darker tail (carpet vs. tile feel).
//   diffusion    (0..1)    scales input-diffuser allpass gains.
//                          Higher ⇒ smoother, less echo-y early signal.
//   dry_gain_db  (-∞..+∞)  passthrough of the source signal at the
//                          effect output. Default 0 dB (unity) so the
//                          reverb on an insert sounds like source + tail.
//                          For send/return on a dedicated reverb bus,
//                          set this very negative (e.g. -60 dB) so the
//                          return path is wet-only.
//   wet_gain_db  (-∞..+∞)  post-effect output trim on the wet field.
//                          (Dock UI bounds: -60..+6 dB.)
//
// v0.29.0 retains EffectParameter::Reverb_RoomSize and Reverb_Damping
// enum IDs as deprecated aliases for Reverb_Decay and Reverb_HfDamping
// (same numeric IDs). Old configs and old API callers continue to work.
//
// Damping implementation note: this release uses stacked one-pole
// shelves — one lowpass for HF damping, one parallel low-cutoff lowpass
// whose output is subtracted for LF damping. A future v0.30.x release
// may upgrade to analytical Cytomic SVFs for cleaner shelf shapes if
// material distinctions require it. The parameter surface is unchanged
// either way.

#ifndef AUDIO_ENGINE_DSP_REVERB_EFFECT_H
#define AUDIO_ENGINE_DSP_REVERB_EFFECT_H

#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/bus.h"

#include <array>
#include <cstdint>
#include <vector>

namespace audio {

class ReverbEffect final : public IDspEffect {
public:
    ReverbEffect(float predelayMs, float decay,
                 float lfDamping, float hfDamping,
                 float diffusion, float dryGainDb, float wetGainDb);

    void Prepare(uint32_t sampleRate, uint32_t channels) override;
    void Process(float* output, uint32_t frames, uint32_t channels,
                 const float* sidechain, uint32_t sidechainChannels) noexcept override;
    void OnParameter(uint16_t paramId, float value) noexcept override;
    uint16_t SidechainBusId() const noexcept override { return kInvalidBusId; }
    EffectKind Kind() const noexcept override { return EffectKind::Reverb; }
    float GetParameter(uint16_t paramId) const noexcept override;

private:
    // ---- Building blocks --------------------------------------------------

    struct DelayLine {
        std::vector<float> buf;
        uint32_t pos = 0;

        inline float Read() const noexcept { return buf[pos]; }

        // Read the sample that was written `offsetBack` samples ago.
        // Used for the named output taps inside the tank.
        inline float ReadTap(uint32_t offsetBack) const noexcept {
            const uint32_t n = static_cast<uint32_t>(buf.size());
            const uint32_t i = (pos + n - (offsetBack % n)) % n;
            return buf[i];
        }

        // Read at fractional sample offset (linear interpolation).
        // Used by ModulatedAllpass to read from a moving tap position.
        inline float ReadFractional(float offsetBack) const noexcept {
            const uint32_t n = static_cast<uint32_t>(buf.size());
            float fpos = static_cast<float>(pos) + static_cast<float>(n) - offsetBack;
            // Bring into [0, n) — offsetBack is bounded by buffer size + modDepth.
            while (fpos < 0.0f)               fpos += static_cast<float>(n);
            while (fpos >= static_cast<float>(n)) fpos -= static_cast<float>(n);
            const uint32_t i0 = static_cast<uint32_t>(fpos);
            const uint32_t i1 = (i0 + 1u) % n;
            const float    frac = fpos - static_cast<float>(i0);
            return buf[i0] * (1.0f - frac) + buf[i1] * frac;
        }

        inline void Write(float x) noexcept { buf[pos] = x; }
        inline void Advance() noexcept {
            pos = (pos + 1u) % static_cast<uint32_t>(buf.size());
        }
        void Reset() noexcept {
            for (auto& v : buf) v = 0.0f;
            pos = 0;
        }
    };

    // Schroeder allpass: y = -gain*x + d; write x + gain*y.
    //
    // Writing back `x + gain*y` (NOT `x + gain*d`) is what makes this a
    // unity-gain allpass. See lessons_learned.md "Schroeder allpass
    // write-back: y, not d" — the wrong form is a comb filter with
    // gain > 1 at low frequencies, which we shipped in v0.29.0-v0.29.3
    // and which broke the tank's stability at decay > ~0.6.
    struct Allpass {
        DelayLine line;
        float gain = 0.5f;

        inline float Step(float x) noexcept {
            const float d = line.Read();
            const float y = -gain * x + d;
            line.Write(x + gain * y);
            line.Advance();
            return y;
        }
        void Reset() noexcept { line.Reset(); }
    };

    // Allpass with LFO-modulated tap read. The read position moves around
    // the nominal delay length by `modDepth` samples driven by a sine LFO.
    // This breaks up the metallic periodicity that static delays produce
    // on sustained tonal material.
    struct ModulatedAllpass {
        DelayLine line;
        float    gain      = -0.7f;
        float    lfoPhase  = 0.0f;     // [0, 1)
        float    lfoIncr   = 0.0f;     // set in Prepare from Hz / sample rate
        float    modDepth  = 8.0f;     // sample variation around baseDelay
        float    baseDelay = 0.0f;     // nominal tap position, samples

        float Step(float x) noexcept;
        void  Reset() noexcept { line.Reset(); lfoPhase = 0.0f; }
    };

    // Stacked one-pole shelves: separable LF and HF damping.
    struct DampingShelf {
        float hfState_ = 0.0f;
        float lfState_ = 0.0f;
        float hfCoef_  = 1.0f;    // 0..1; higher = pass more highs (less HF damp)
        float lfCoef_  = 0.05f;   // small; LF tracking cutoff coefficient
        float lfDamp_  = 0.0f;    // 0..1, fraction of LF baseline to subtract

        void Configure(float lfDamping, float hfDamping,
                       uint32_t sampleRate) noexcept;

        inline float Step(float x) noexcept {
            // HF damp: lowpass smoothing. hfCoef==1.0 ⇒ full pass-through.
            hfState_ = hfCoef_ * x + (1.0f - hfCoef_) * hfState_;
            // LF tracking: very-low-cutoff LP follows the slow component.
            lfState_ = lfCoef_ * hfState_ + (1.0f - lfCoef_) * lfState_;
            // Output = HF-damped signal minus the subtractive LF component.
            return hfState_ - lfDamp_ * lfState_;
        }
        void Reset() noexcept { hfState_ = 0.0f; lfState_ = 0.0f; }
    };

    struct TankHalf {
        ModulatedAllpass modAP;
        DelayLine        delay1;
        DampingShelf     damping;
        Allpass          ap;
        DelayLine        delay2;
        void Reset() noexcept {
            modAP.Reset(); delay1.Reset(); damping.Reset();
            ap.Reset();    delay2.Reset();
        }
    };

    // ---- Derived value recompute ----------------------------------------

    void RecomputeWetGain() noexcept;
    void RecomputeDryGain() noexcept;
    void RecomputeDecayFeedback() noexcept;
    void RecomputeDiffusion() noexcept;
    void RecomputePredelay() noexcept;
    void RecomputeDamping() noexcept;

    // ---- Parameter targets (canonical state) ----------------------------

    float predelayMs_ = 30.0f;
    float decay_      = 0.5f;
    float lfDamping_  = 0.0f;
    float hfDamping_  = 0.3f;
    float diffusion_  = 0.625f;
    float dryGainDb_  = 0.0f;
    float wetGainDb_  = 0.0f;

    // ---- Derived render-thread values -----------------------------------

    float    tankFeedback_   = 0.5f;
    float    diffuserGainAB_ = 0.75f;
    float    diffuserGainCD_ = 0.625f;
    float    dryLin_         = 1.0f;
    float    wetLin_         = 1.0f;
    uint32_t predelaySamples_ = 0;

    uint32_t sampleRate_ = 48000;
    uint32_t channels_   = 2;

    // ---- Audio paths ----------------------------------------------------

    DelayLine                predelay_;
    std::array<Allpass, 4>   inputDiffuser_;
    std::array<TankHalf, 2>  tank_;

    // Cross-coupling state: feeds one tank half's output back into the
    // other half's input on the next sample.
    float crossFromA_ = 0.0f;
    float crossFromB_ = 0.0f;
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_REVERB_EFFECT_H
