// audio_engine/dsp/dsp_effect.h
//
// Render-thread DSP effect interface. Effects live inside a bus's effect
// chain and process the bus's mixed buffer in-place. They may read from a
// sidechain buffer (another bus's output buffer) when the configuration
// declares one.
//
// Render-thread contract:
//   * Process() must not allocate, lock, throw, or do I/O.
//   * Prepare() is called once at Initialize() and is allowed to allocate.
//   * OnParameter() is called from the render thread between Process() calls;
//     effects should snapshot a target value and ramp internally if they
//     care about click-free transitions.

#ifndef AUDIO_ENGINE_DSP_DSP_EFFECT_H
#define AUDIO_ENGINE_DSP_DSP_EFFECT_H

#include <cstdint>

namespace audio {

class IDspEffect {
public:
    virtual ~IDspEffect() = default;

    // Called once before the first Process(). May allocate.
    virtual void Prepare(uint32_t sampleRate, uint32_t channels) = 0;

    // Called per render callback. Processes `output` in place. If this
    // effect declares a sidechain reference, `sidechain` points to a
    // contiguous buffer of `frames * sidechainChannels` floats holding the
    // sidechain bus's output for this same callback; otherwise it is null.
    virtual void Process(float*       output,
                          uint32_t     frames,
                          uint32_t     channels,
                          const float* sidechain,
                          uint32_t     sidechainChannels) noexcept = 0;

    // Update a named parameter. paramId values are in EffectParameter::*.
    // Ignored if the effect doesn't recognize the id.
    virtual void OnParameter(uint16_t paramId, float value) noexcept = 0;

    // Returns the sidechain bus this effect reads from, or kInvalidBusId.
    virtual uint16_t SidechainBusId() const noexcept = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_DSP_EFFECT_H
