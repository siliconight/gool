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

#include "audio_engine/bus.h"  // EffectKind

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

    // v0.28.0: introspection for live effect-chain editing.
    //
    // Kind(): returns this effect's EffectKind enum value so a host
    // (the mixer dock, an editor script) knows what kind of effect
    // sits at each effect-chain position without having to track it
    // out-of-band. Stable for the effect's lifetime.
    //
    // GetParameter(paramId): returns the current target value of the
    // named parameter (the value that OnParameter would set, NOT
    // necessarily the current per-sample ramped value). Returns 0.0f
    // for unrecognized parameter IDs — symmetric with OnParameter's
    // "ignored if not recognized" behavior.
    //
    // Thread safety: GetParameter is called from the control thread
    // while the render thread may be writing the same fields via
    // OnParameter. Reads of single-word float members are atomic on
    // x86 and ARM (the platforms gool targets), so the worst case is
    // observing a value that's one callback behind a concurrent set
    // — fine for the UI-display use case. No formal atomic<float>
    // upgrade is required.
    virtual EffectKind Kind() const noexcept = 0;
    virtual float GetParameter(uint16_t paramId) const noexcept = 0;
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_DSP_EFFECT_H
