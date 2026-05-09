// audio_engine/dsp/gain_effect.h
//
// Linear gain with internal slew. Parameter changes ramp toward the target
// over a fixed window (`kRampMs`) to avoid zipper noise and click artifacts.

#ifndef AUDIO_ENGINE_DSP_GAIN_EFFECT_H
#define AUDIO_ENGINE_DSP_GAIN_EFFECT_H

#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/bus.h"

namespace audio {

class GainEffect final : public IDspEffect {
public:
    explicit GainEffect(float initialGainDb);

    void Prepare(uint32_t sampleRate, uint32_t channels) override;
    void Process(float* output, uint32_t frames, uint32_t channels,
                 const float* sidechain, uint32_t sidechainChannels) noexcept override;
    void OnParameter(uint16_t paramId, float value) noexcept override;
    uint16_t SidechainBusId() const noexcept override { return kInvalidBusId; }

private:
    static constexpr float kRampMs = 5.0f;

    float    current_    = 1.0f;
    float    target_     = 1.0f;
    uint32_t sampleRate_ = 48000;
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_GAIN_EFFECT_H
