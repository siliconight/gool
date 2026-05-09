// audio_engine/dsp/compressor.h
//
// Dynamic range compressor with optional sidechain input. Standard topology:
//
//   input -> [envelope follower] -> [static gain computer] -> gain reduction
//                  ^
//                  |
//             sidechain (or input if self-sidechained)
//
// Envelope follower: peak detector with separate attack/release time
// constants. Gain computer: hard-knee threshold + ratio with makeup gain.
//
// When `compressorSidechainBus` is set, the engine routes that bus's output
// buffer to Process()'s `sidechain` pointer. The compressor then derives its
// envelope from the sidechain signal but applies the resulting gain
// reduction to the main signal it's processing; which is the entire point
// of ducking ("compress Music when LocalGun fires").

#ifndef AUDIO_ENGINE_DSP_COMPRESSOR_H
#define AUDIO_ENGINE_DSP_COMPRESSOR_H

#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/bus.h"

namespace audio {

class CompressorEffect final : public IDspEffect {
public:
    CompressorEffect(float thresholdDb,
                      float ratio,
                      float attackMs,
                      float releaseMs,
                      float makeupDb,
                      BusId sidechainBus);

    void Prepare(uint32_t sampleRate, uint32_t channels) override;
    void Process(float* output, uint32_t frames, uint32_t channels,
                 const float* sidechain, uint32_t sidechainChannels) noexcept override;
    void OnParameter(uint16_t paramId, float value) noexcept override;

    uint16_t SidechainBusId() const noexcept override {
        return static_cast<uint16_t>(sidechainBus_);
    }

    // For diagnostics / unit tests.
    float CurrentEnvelopeDb()  const noexcept { return envelopeDb_; }
    float CurrentReductionDb() const noexcept { return lastReductionDb_; }

private:
    // Configuration (target values).
    float thresholdDb_;
    float ratio_;
    float attackMs_;
    float releaseMs_;
    float makeupDb_;
    BusId sidechainBus_;

    // Derived state.
    uint32_t sampleRate_ = 48000;
    float    attackCoeff_  = 0.0f;        // smoothing coefficient per sample
    float    releaseCoeff_ = 0.0f;
    bool     coeffDirty_   = true;

    // Envelope state in dB (running peak smoothed by attack/release).
    float envelopeDb_      = -120.0f;
    float lastReductionDb_ = 0.0f;
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_COMPRESSOR_H
