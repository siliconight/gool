// tests/unit/memory_budget_test.cpp
//
// Sanity tests for EstimateBaselineBytes. Verifies the estimator
// produces sensible numbers and responds monotonically to scaling
// inputs (more voices → more bytes, larger buffer → more bytes).

#include "audio_engine/memory_budget.h"
#include "audio_engine/config.h"

#include <cassert>
#include <cstdio>

int main() {
    // Default config baseline.
    audio::AudioConfig cfg{};
    const uint64_t baseline = audio::EstimateBaselineBytes(cfg);

    // Should be non-zero and within a sane range. Default-config baseline
    // is around 3-5 MB; well above 64 KB and well below 1 GB.
    assert(baseline > 64ull * 1024ull);
    assert(baseline < 1024ull * 1024ull * 1024ull);
    std::printf("  default baseline:        %llu bytes (~%.1f MB)\n",
                static_cast<unsigned long long>(baseline),
                static_cast<double>(baseline) / (1024.0 * 1024.0));

    // Monotonic in maxActiveEmitters: doubling emitters increases bytes.
    audio::AudioConfig cfgMoreEmitters = cfg;
    cfgMoreEmitters.budget.maxActiveEmitters = cfg.budget.maxActiveEmitters * 4u;
    const uint64_t moreEmitters = audio::EstimateBaselineBytes(cfgMoreEmitters);
    assert(moreEmitters > baseline);
    std::printf("  4x emitters baseline:    %llu bytes (Δ=%+lld)\n",
                static_cast<unsigned long long>(moreEmitters),
                static_cast<long long>(moreEmitters) - static_cast<long long>(baseline));

    // Monotonic in bufferSize: doubling buffer size increases bus buffer bytes.
    audio::AudioConfig cfgBigBuffer = cfg;
    cfgBigBuffer.bufferSize = cfg.bufferSize * 2u;
    const uint64_t bigBuffer = audio::EstimateBaselineBytes(cfgBigBuffer);
    assert(bigBuffer > baseline);

    // Disabling voice reduces bytes (voice rings drop to zero).
    audio::AudioConfig cfgNoVoice = cfg;
    cfgNoVoice.enableVoice = false;
    const uint64_t noVoice = audio::EstimateBaselineBytes(cfgNoVoice);
    assert(noVoice < baseline);
    std::printf("  baseline without voice:  %llu bytes (Δ=%lld)\n",
                static_cast<unsigned long long>(noVoice),
                static_cast<long long>(noVoice) - static_cast<long long>(baseline));

    // Monotonic in maxRegisteredSounds (small contribution but should be non-negative).
    audio::AudioConfig cfgMoreSounds = cfg;
    cfgMoreSounds.budget.maxRegisteredSounds = cfg.budget.maxRegisteredSounds * 4u;
    const uint64_t moreSounds = audio::EstimateBaselineBytes(cfgMoreSounds);
    assert(moreSounds >= baseline);

    // Pure-function property: same input → same output.
    assert(audio::EstimateBaselineBytes(cfg) == baseline);
    assert(audio::EstimateBaselineBytes(cfg) == baseline);

    std::printf("[memory_budget_test] PASSED\n");
    return 0;
}
