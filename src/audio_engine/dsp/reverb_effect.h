// audio_engine/dsp/reverb_effect.h
//
// Freeverb-derived stereo algorithmic reverb. 8 parallel comb filters with
// per-comb damping LPFs feeding 4 series allpass filters per stereo
// channel; the right-channel delay lengths are offset by ~23 samples
// (scaled to the operating sample rate) to produce a stereo tail from a
// stereo input.
//
// Two parameters drive the perception:
//   roomSize  0..1   feedback gain in the comb loops; longer roomsize ⇒
//                    slower decay ⇒ "bigger room"
//   damping   0..1   one-pole LPF coefficient inside each comb's feedback
//                    path; higher damping ⇒ darker tail ⇒ "softer
//                    surfaces"
//
// The effect is intended to live on a dedicated send bus that voices send
// into via SpatialParams.reverbSend. It runs wet-only; the input arrives
// already pre-scaled by the per-voice send level, and the dry path is
// preserved by the voice's normal target-bus mix. wetGainDb is a
// post-effect trim for matching levels with the dry mix.
//
// Public-domain algorithm (Freeverb, Jezar at Dreampoint, 2000). This is
// a from-scratch C++20 implementation; the constants below are the
// canonical Freeverb tunings.

#ifndef AUDIO_ENGINE_DSP_REVERB_EFFECT_H
#define AUDIO_ENGINE_DSP_REVERB_EFFECT_H

#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/bus.h"

#include <array>
#include <vector>

namespace audio {

class ReverbEffect final : public IDspEffect {
public:
    ReverbEffect(float roomSize, float damping, float wetGainDb);

    void Prepare(uint32_t sampleRate, uint32_t channels) override;
    void Process(float* output, uint32_t frames, uint32_t channels,
                 const float* sidechain, uint32_t sidechainChannels) noexcept override;
    void OnParameter(uint16_t paramId, float value) noexcept override;
    uint16_t SidechainBusId() const noexcept override { return kInvalidBusId; }

private:
    static constexpr uint32_t kCombs    = 8;
    static constexpr uint32_t kAllpass  = 4;

    // Freeverb canonical delay lengths in samples at 44.1 kHz, separately
    // for left and right channels (the right channel uses an extra 23-sample
    // offset to broaden the stereo image).
    static constexpr uint32_t kCombsLengths44[kCombs] = {
        1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
    };
    static constexpr uint32_t kAllpassLengths44[kAllpass] = {
        556, 441, 341, 225
    };
    static constexpr uint32_t kStereoSpread = 23;     // samples at 44.1 kHz

    struct Comb {
        std::vector<float> buf;
        uint32_t pos = 0;
        float    filterStore = 0.0f;
        // returns y, advances state. damp in [0,1) maps to feedback LPF
        // coefficient: filterStore = filterStore * damp + buf[pos] * (1-damp).
        inline float Step(float input, float feedback, float damp) noexcept {
            const float out = buf[pos];
            filterStore = out * (1.0f - damp) + filterStore * damp;
            buf[pos]    = input + filterStore * feedback;
            pos = (pos + 1u) % static_cast<uint32_t>(buf.size());
            return out;
        }
        void Reset() noexcept {
            for (auto& v : buf) v = 0.0f;
            pos = 0;
            filterStore = 0.0f;
        }
    };

    struct Allpass {
        std::vector<float> buf;
        uint32_t pos = 0;
        // Schroeder allpass: y = -input + buf[pos]; buf[pos] = input + buf[pos]*0.5.
        inline float Step(float input) noexcept {
            const float bufout = buf[pos];
            const float y      = -input + bufout;
            buf[pos]           = input + bufout * 0.5f;
            pos = (pos + 1u) % static_cast<uint32_t>(buf.size());
            return y;
        }
        void Reset() noexcept {
            for (auto& v : buf) v = 0.0f;
            pos = 0;
        }
    };

    void RecomputeWetGain() noexcept;

    float roomSize_  = 0.7f;
    float damping_   = 0.5f;
    float wetGainDb_ = 0.0f;
    float feedback_  = 0.84f;     // = 0.7 * 0.28 + 0.7 (Freeverb scaling)
    float dampVal_   = 0.4f;      // = damping * 0.4 (Freeverb scaling)
    float wetLin_    = 1.0f;

    uint32_t sampleRate_ = 48000;
    uint32_t channels_   = 2;

    std::array<Comb,    kCombs>   combL_{}, combR_{};
    std::array<Allpass, kAllpass> apL_{},   apR_{};
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_REVERB_EFFECT_H
