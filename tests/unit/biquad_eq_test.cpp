// tests/unit/biquad_eq_test.cpp
//
// Validates the LowShelf, HighShelf, and Peak biquad types added
// to BiquadFilterEffect. We test the filter directly (no runtime)
// so the cookbook coefficient math is the only thing under test.
//
// For each filter type we generate two reference sines at a low
// and a high frequency, push each through the filter, and measure
// the gain in dB. A LowShelf at +12 dB / 200 Hz should:
//   - boost a 100 Hz sine by ~12 dB,
//   - leave a 5 kHz sine roughly unchanged.
// HighShelf and Peak follow the symmetric pattern. Tolerances are
// generous because filter rolloff isn't a brick wall — what we're
// proving is the cookbook math gives the right shape.

#include "audio_engine/dsp/biquad_filter.h"
#include "audio_engine/bus.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr uint32_t kSr = 48000;

// Generate `numFrames` of an interleaved-stereo sine at `freq` Hz,
// amplitude `amp`. The biquad processes interleaved data in place.
std::vector<float> StereoSine(uint32_t numFrames, float freq, float amp = 0.5f) {
    std::vector<float> v(numFrames * 2);
    for (uint32_t i = 0; i < numFrames; ++i) {
        const float s = amp * std::sin(2.0f * kPi * freq *
                                         static_cast<float>(i) /
                                         static_cast<float>(kSr));
        v[i * 2 + 0] = s;
        v[i * 2 + 1] = s;
    }
    return v;
}

// RMS of left channel of an interleaved-stereo block, skipping the
// first `skip` frames (lets transient settle).
float RmsLeft(const std::vector<float>& v, uint32_t skip = 0) {
    if (v.size() < 4) return 0.0f;
    double s = 0.0;
    size_t n = 0;
    for (size_t i = skip * 2; i + 1 < v.size(); i += 2) {
        s += v[i] * v[i];
        ++n;
    }
    return static_cast<float>(std::sqrt(s / std::max<size_t>(1, n)));
}

// Run a sine through the given filter, return gain in dB (output / input).
float MeasureGainDb(audio::BiquadFilterEffect& filter, float freq) {
    constexpr uint32_t kFrames = 8192;        // ~170 ms; plenty for steady state
    constexpr uint32_t kSkip   = 2048;         // skip the impulse response transient
    auto signal = StereoSine(kFrames, freq);
    const float inputRms = RmsLeft(signal, kSkip);
    filter.Process(signal.data(), kFrames, /*channels=*/2u, nullptr, 0u);
    const float outputRms = RmsLeft(signal, kSkip);
    if (inputRms < 1e-6f) return 0.0f;
    return 20.0f * std::log10(outputRms / inputRms);
}

void TestLowShelfBoost() {
    std::cout << "  [low shelf @ 200 Hz, +12 dB]\n";
    audio::BiquadFilterEffect filter(audio::BiquadType::LowShelf,
                                       /*cutoffHz=*/200.0f,
                                       /*Q=*/0.707f,
                                       /*gainDb=*/12.0f);
    filter.Prepare(kSr, 2);

    const float gainLo = MeasureGainDb(filter, 60.0f);     // well below cutoff
    filter.Prepare(kSr, 2);                                 // reset state
    const float gainHi = MeasureGainDb(filter, 5000.0f);    // well above cutoff

    std::printf("    60 Hz   : %+.2f dB (expect ≈ +12)\n", gainLo);
    std::printf("    5000 Hz : %+.2f dB (expect ≈ 0)\n",   gainHi);
    std::fflush(stdout);
    assert(std::abs(gainLo - 12.0f) < 1.5f);
    assert(std::abs(gainHi)        < 1.5f);
}

void TestHighShelfBoost() {
    std::cout << "  [high shelf @ 2 kHz, +12 dB]\n";
    audio::BiquadFilterEffect filter(audio::BiquadType::HighShelf,
                                       /*cutoffHz=*/2000.0f,
                                       /*Q=*/0.707f,
                                       /*gainDb=*/12.0f);
    filter.Prepare(kSr, 2);

    const float gainLo = MeasureGainDb(filter, 100.0f);
    filter.Prepare(kSr, 2);
    const float gainHi = MeasureGainDb(filter, 8000.0f);

    std::printf("    100 Hz  : %+.2f dB (expect ≈ 0)\n", gainLo);
    std::printf("    8000 Hz : %+.2f dB (expect ≈ +12)\n", gainHi);
    std::fflush(stdout);
    assert(std::abs(gainLo)        < 1.5f);
    assert(std::abs(gainHi - 12.0f) < 1.5f);
}

void TestPeakBoost() {
    std::cout << "  [peak @ 1 kHz, +12 dB, Q=2]\n";
    audio::BiquadFilterEffect filter(audio::BiquadType::Peak,
                                       /*cutoffHz=*/1000.0f,
                                       /*Q=*/2.0f,
                                       /*gainDb=*/12.0f);
    filter.Prepare(kSr, 2);

    const float gainAt   = MeasureGainDb(filter, 1000.0f);
    filter.Prepare(kSr, 2);
    const float gainBelow = MeasureGainDb(filter, 100.0f);
    filter.Prepare(kSr, 2);
    const float gainAbove = MeasureGainDb(filter, 8000.0f);

    std::printf("    100 Hz  : %+.2f dB (expect ≈ 0)\n",   gainBelow);
    std::printf("    1000 Hz : %+.2f dB (expect ≈ +12)\n", gainAt);
    std::printf("    8000 Hz : %+.2f dB (expect ≈ 0)\n",   gainAbove);
    std::fflush(stdout);
    assert(std::abs(gainBelow) < 1.5f);
    assert(std::abs(gainAt - 12.0f) < 1.5f);
    assert(std::abs(gainAbove) < 1.5f);
}

void TestPeakCut() {
    std::cout << "  [peak @ 1 kHz, -12 dB, Q=2]\n";
    audio::BiquadFilterEffect filter(audio::BiquadType::Peak,
                                       1000.0f, 2.0f, -12.0f);
    filter.Prepare(kSr, 2);

    const float gainAt = MeasureGainDb(filter, 1000.0f);
    std::printf("    1000 Hz : %+.2f dB (expect ≈ -12)\n", gainAt);
    std::fflush(stdout);
    assert(std::abs(gainAt + 12.0f) < 1.5f);
}

void TestUnityZeroDb() {
    std::cout << "  [shelves and peak at 0 dB are unity transfer]\n";
    for (auto type : {audio::BiquadType::LowShelf,
                       audio::BiquadType::HighShelf,
                       audio::BiquadType::Peak}) {
        audio::BiquadFilterEffect filter(type, 1000.0f, 1.0f, /*gainDb=*/0.0f);
        filter.Prepare(kSr, 2);
        const float g = MeasureGainDb(filter, 1000.0f);
        std::printf("    type=%d at 1000 Hz: %+.4f dB (expect ≈ 0)\n",
                     static_cast<int>(type), g);
        std::fflush(stdout);
        assert(std::abs(g) < 0.1f);
    }
}

void TestRuntimeParameterChange() {
    std::cout << "  [Biquad_GainDb live update via OnParameter]\n";
    audio::BiquadFilterEffect filter(audio::BiquadType::Peak,
                                       1000.0f, 2.0f, /*gainDb=*/0.0f);
    filter.Prepare(kSr, 2);
    const float gainStart = MeasureGainDb(filter, 1000.0f);
    filter.OnParameter(audio::EffectParameter::Biquad_GainDb, 6.0f);
    filter.Prepare(kSr, 2);   // reset state, recompute coeffs
    const float gainAfter = MeasureGainDb(filter, 1000.0f);
    std::printf("    before: %+.2f dB, after Biquad_GainDb=6: %+.2f dB\n",
                 gainStart, gainAfter);
    std::fflush(stdout);
    assert(std::abs(gainStart) < 1.5f);
    assert(std::abs(gainAfter - 6.0f) < 1.5f);
}

} // namespace

int main() {
    std::cout << "[biquad_eq_test] running...\n";
    TestLowShelfBoost();
    TestHighShelfBoost();
    TestPeakBoost();
    TestPeakCut();
    TestUnityZeroDb();
    TestRuntimeParameterChange();
    std::cout << "[biquad_eq_test] OK\n";
    return 0;
}
