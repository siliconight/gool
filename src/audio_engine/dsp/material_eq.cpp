// src/audio_engine/dsp/material_eq.cpp
//
// Implementation of audio_engine/material_eq.h.
//
// Lives under src/ so the public header doesn't have to expose
// BiquadFilterEffect (which is still an internal class). The
// audition's biquad chain is built here and run in place over
// the caller's buffer.

#include "audio_engine/material_eq.h"

#include "audio_engine/dsp/biquad_filter.h"   // private impl, OK from src/

#include <cstdint>

namespace audio {

void ProcessBufferThroughMaterialEqCurve(
    float* samples,
    std::size_t numSamples,
    const MaterialEqCurve& curve,
    float intensity,
    std::uint32_t sampleRate) noexcept {
    if (samples == nullptr || numSamples == 0) {
        return;
    }
    if (sampleRate == 0) {
        sampleRate = 48000;
    }

    // Construct the three biquads with intensity-scaled gains.
    // Shelf Q fixed at 1.0 (matches runtime impact/listener chains
    // and the v0.59.3 binding behavior). Peak Q from the curve.
    BiquadFilterEffect low(
        BiquadType::LowShelf,
        curve.lowFreqHz,  1.0f,
        curve.lowGainDb  * intensity);
    BiquadFilterEffect mid(
        BiquadType::Peak,
        curve.midFreqHz,  curve.midQ,
        curve.midGainDb  * intensity);
    BiquadFilterEffect high(
        BiquadType::HighShelf,
        curve.highFreqHz, 1.0f,
        curve.highGainDb * intensity);

    low.Prepare(sampleRate, 1);
    mid.Prepare(sampleRate, 1);
    high.Prepare(sampleRate, 1);

    // Process in place. BiquadFilterEffect::Process is in-place by
    // design (output buffer is also input), so the chain runs as
    // three sequential passes over the same memory.
    const std::uint32_t n = static_cast<std::uint32_t>(numSamples);
    low.Process(samples,  n, 1, nullptr, 0);
    mid.Process(samples,  n, 1, nullptr, 0);
    high.Process(samples, n, 1, nullptr, 0);
}

void ProcessBufferThroughMaterialEq(
    float* samples,
    std::size_t numSamples,
    AudioMaterial material,
    float intensity,
    std::uint32_t sampleRate) noexcept {
    // Resolve material → curve. The whole function is noexcept;
    // MaterialEqByMaterial is itself noexcept and total over the
    // AudioMaterial enum range. Out-of-range ints (e.g. cast from
    // a bad GDScript int) are caught by the caller boundary — the
    // binding bounds-checks before the static_cast — but should
    // any other caller pass an out-of-range value, the switch in
    // MaterialEqByMaterial falls through to AudioMaterial::Default,
    // producing a neutral passthrough audition. Safe by default.
    const MaterialEqCurve curve = MaterialEqByMaterial(material);
    ProcessBufferThroughMaterialEqCurve(
        samples, numSamples, curve, intensity, sampleRate);
}

} // namespace audio
