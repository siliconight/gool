// audio_engine/memory_budget.h
//
// Baseline memory-budget estimator for an AudioRuntime configuration.
// Used to populate AudioRuntime::Stats::approxBytesAllocated so users
// can see at a glance roughly how many bytes the engine will hold
// resident under the current config.
//
// SCOPE OF THE ESTIMATE:
//
// EstimateBaselineBytes covers the major allocation categories that
// are fully determined by the AudioConfig + AudioRuntimeBudget:
//
//   * Bus graph input/output buffers (assumes ~16 typical buses)
//   * Voice mix pool (MixVoice array + binaural delay lines)
//   * Voice-source manager rings (PCM ring + packet ring per source)
//   * Streaming asset rings (one float ring per streaming asset)
//   * Asset registry hash tables (rough bucket overhead)
//
// IT DOES NOT COVER:
//
//   * Loaded PCM asset bytes (RegisterPcm content — varies wildly
//     by game; can dwarf the baseline)
//   * Sound bank parser/runtime tables (typically <100KB)
//   * Effect-internal state (reverb delay lines, biquad filter memory,
//     compressor envelope state — collectively <1MB for typical
//     bus graphs)
//   * Standard library overhead (string interning, etc.)
//   * Anything user-installed via deps (telemetry/log sinks, custom
//     spatializers, custom backends).
//
// The estimate is INTENTIONALLY CONSERVATIVE-LOW (often by 1.5-3x of
// real usage), so users tracking memory in production should always
// allow generous headroom over what this returns.
//
// PURE FUNCTION: no allocation, no I/O. Safe to call from any thread.
// Cost: ~10 multiplications. Cheap enough to call every GetStats().

#ifndef AUDIO_ENGINE_MEMORY_BUDGET_H
#define AUDIO_ENGINE_MEMORY_BUDGET_H

#include <cstdint>

namespace audio {

struct AudioConfig;

// Returns an approximate resident-byte count for an engine initialized
// with `config`. See header comment for scope and limitations.
uint64_t EstimateBaselineBytes(const AudioConfig& config) noexcept;

} // namespace audio

#endif // AUDIO_ENGINE_MEMORY_BUDGET_H
