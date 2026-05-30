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

// audio_engine/dsp/biquad_filter.h
//
// Per-channel biquad filter (LPF / HPF / BPF) using Robert Bristow-Johnson's
// audio cookbook coefficients. Coefficients are recomputed lazily on
// parameter change. Per-channel state is maintained for true stereo
// processing (no channel coupling).
//
// Parameter changes do not crossfade; abrupt cutoff jumps may produce a
// short click. For modulation use cases (e.g. an explosion's concussion
// sweep) drive the cutoff with a smoothed source on the control thread.

#ifndef AUDIO_ENGINE_DSP_BIQUAD_FILTER_H
#define AUDIO_ENGINE_DSP_BIQUAD_FILTER_H

#include <array>

#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/bus.h"

namespace audio {

class BiquadFilterEffect final : public IDspEffect {
public:
    // gainDb is honored only by LowShelf, HighShelf, and Peak; the
    // basic LP/HP/BP types ignore it. Default 0 dB = unity (no boost
    // or cut), which keeps backward compatibility for existing
    // callers that pass two-arg form.
    BiquadFilterEffect(BiquadType type, float cutoffHz, float Q,
                        float gainDb = 0.0f);

    void Prepare(uint32_t sampleRate, uint32_t channels) override;
    void Process(float* output, uint32_t frames, uint32_t channels,
                 const float* sidechain, uint32_t sidechainChannels) noexcept override;
    void OnParameter(uint16_t paramId, float value) noexcept override;
    uint16_t SidechainBusId() const noexcept override { return kInvalidBusId; }
    // v0.28.0: introspection.
    EffectKind Kind() const noexcept override { return EffectKind::BiquadFilter; }
    float GetParameter(uint16_t paramId) const noexcept override;

private:
    void RecomputeCoefficients() noexcept;

    BiquadType type_;
    float      cutoffHz_;
    float      Q_;
    float      gainDb_ = 0.0f;

    uint32_t sampleRate_ = 48000;
    bool     dirty_      = true;

    // Direct-form II transposed coefficients.
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f;
    float a1_ = 0.0f, a2_ = 0.0f;

    // Per-channel state. We support up to 8 channels; render path uses only
    // the first `channels_` entries.
    static constexpr uint32_t kMaxChannels = 8;
    std::array<float, kMaxChannels> z1_{};
    std::array<float, kMaxChannels> z2_{};
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_BIQUAD_FILTER_H
