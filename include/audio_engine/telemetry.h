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

// include/audio_engine/telemetry.h
//
// Runtime telemetry sink interface plus three built-in implementations.
//
// The runtime publishes a `Stats` snapshot on every Update tick. By
// default that snapshot is only readable via AudioRuntime::GetStats()
// — fine for in-process use, less convenient for piping into a
// monitoring pipeline. A telemetry sink wires the same data to wherever
// the host needs it (Prometheus, Datadog, journald, custom analytics)
// at a configurable cadence, without polluting the runtime's hot path.
//
// The sink is called from the game thread, inside Update(), at the
// interval set by AudioConfig::telemetryIntervalMs. The runtime stores
// the sink pointer verbatim from AudioRuntimeDependencies — lifetime
// is host-managed, never deleted by the runtime.
//
// This header is the only one external sink implementations need to
// include. Sinks compile and link against the public engine library;
// no internal-impl headers are required.

#ifndef AUDIO_ENGINE_TELEMETRY_H
#define AUDIO_ENGINE_TELEMETRY_H

#include "audio_engine/audio_runtime.h"
#include "audio_engine/types.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace audio {

// One telemetry sample. The runtime constructs and passes this each
// emit interval; it borrows the Stats by reference for the duration
// of the call. Sinks that need to keep the sample (e.g. RingTelemetrySink)
// must copy.
struct RuntimeStatsSample {
    // Control-thread wall clock at the moment the sample was published.
    // Monotonic across a single AudioRuntime instance. Resets on
    // Initialize. Use this rather than wall clock for time-series x-axis
    // because it advances by deltaSeconds each Update — predictable
    // even if the host process is descheduled.
    TimestampMs               timestampMs = 0;

    // Reference to the runtime's latest Stats. Only valid for the
    // duration of the OnRuntimeStats() call. Sinks that store samples
    // must deep-copy (the Stats struct is trivially copyable).
    const AudioRuntime::Stats& stats;
};

// Sink interface. Implementations register via
// AudioRuntimeDependencies::telemetrySink, called from the game thread
// inside Update().
class IRuntimeTelemetrySink {
public:
    virtual ~IRuntimeTelemetrySink() = default;

    // Called once per emit interval with the latest stats snapshot.
    // Implementations should be cheap; the runtime blocks here for
    // the duration of the call. Allocations are discouraged on the
    // hot path — the built-in sinks pre-size their buffers.
    virtual void OnRuntimeStats(const RuntimeStatsSample& sample) = 0;
};

// =============================================================================
// JSON Lines sink
// =============================================================================
//
// Writes one JSON object per stats sample, newline-terminated. Suitable
// for piping into journald, fluentd, vector, or a plain log file:
//
//     auto sink = std::make_unique<audio::JsonLinesTelemetrySink>(stdout);
//     deps.telemetrySink = sink.get();
//     cfg.telemetryIntervalMs = 1000;
//
// Each line is compact (no pretty-printing) and stable: field order
// is deterministic, every key is always emitted, scalars never use
// scientific notation. This makes downstream parsing reliable across
// log shippers that don't tolerate variable schemas.
class JsonLinesTelemetrySink final : public IRuntimeTelemetrySink {
public:
    // The sink writes to `out` and never closes it. Pass stdout for
    // simple scripting, or fopen() a file the host owns. Pass nullptr
    // to silently no-op (useful for tests that want to share a
    // single sink between configurations).
    explicit JsonLinesTelemetrySink(std::FILE* out = stdout) noexcept
        : out_(out) {}

    void OnRuntimeStats(const RuntimeStatsSample& sample) override;

private:
    std::FILE* out_ = nullptr;
};

// =============================================================================
// Prometheus exposition-format sink
// =============================================================================
//
// Maintains a thread-safe text snapshot in Prometheus exposition format
// (https://prometheus.io/docs/instrumenting/exposition_formats/). Each
// OnRuntimeStats call rewrites the snapshot; the host's HTTP server
// calls Snapshot() to serve scrape requests:
//
//     auto sink = std::make_shared<audio::PrometheusTelemetrySink>("gool");
//     // game thread:
//     deps.telemetrySink = sink.get();
//     cfg.telemetryIntervalMs = 1000;
//     // HTTP scrape handler thread:
//     std::string body = sink->Snapshot();
//
// Counters end with `_total` per Prometheus convention; gauges are
// point-in-time values without that suffix. Metric names use the
// `gool_` prefix; the `job` label is set from the constructor argument.
class PrometheusTelemetrySink final : public IRuntimeTelemetrySink {
public:
    explicit PrometheusTelemetrySink(std::string job = "gool") noexcept
        : job_(std::move(job)) {}

    void OnRuntimeStats(const RuntimeStatsSample& sample) override;

    // Returns the latest exposition-format text. Safe to call from any
    // thread; takes a brief lock. Returns an empty string before the
    // first stats emit.
    std::string Snapshot() const;

private:
    mutable std::mutex mutex_;
    std::string        job_;
    std::string        snapshot_;
};

// =============================================================================
// Ring buffer sink (in-memory time series)
// =============================================================================
//
// Keeps the last N samples in a circular buffer. Useful for in-game
// debug overlays, replay correlation, and time-series queries that
// don't need to leave the process:
//
//     auto sink = std::make_unique<audio::RingTelemetrySink>(512);
//     deps.telemetrySink = sink.get();
//     cfg.telemetryIntervalMs = 250;
//     // Later, from any thread:
//     for (const auto& s : sink->Snapshot()) {
//         draw_overlay(s.timestampMs, s.stats.activeEmitters);
//     }
//
// Thread-safe: writes from the runtime (game thread only — telemetry
// emission is always game-thread) and reads via Snapshot() / Size() /
// Capacity() / Clear() from any thread are serialized by an internal
// mutex. ForEachInOrder() is intentionally NOT locked because the
// caller may want to inspect each sample without holding the mutex
// across a long iteration; it is safe ONLY when the caller can
// guarantee no concurrent OnRuntimeStats call (typical: same thread
// as the runtime's Update). Use Snapshot() if you need cross-thread
// inspection.
class RingTelemetrySink final : public IRuntimeTelemetrySink {
public:
    explicit RingTelemetrySink(size_t capacity = 512) {
        buf_.resize(std::max<size_t>(capacity, 1));
    }

    void OnRuntimeStats(const RuntimeStatsSample& sample) override;

    // One stored sample. Same as RuntimeStatsSample but owns its
    // Stats copy (the original holds a reference whose lifetime is
    // the OnRuntimeStats call only).
    struct StoredSample {
        TimestampMs         timestampMs = 0;
        AudioRuntime::Stats stats{};
    };

    // Snapshot the buffer in chronological order (oldest first).
    // Allocates a vector on each call. Thread-safe.
    std::vector<StoredSample> Snapshot() const;

    // Iterate samples in chronological order (oldest first) without
    // allocating. Caller must guarantee no concurrent
    // OnRuntimeStats — typically only safe to call from the same
    // thread that drives Update, or while emission is paused.
    template <typename F>
    void ForEachInOrder(F&& fn) const {
        if (size_ == 0) return;
        const size_t cap = buf_.size();
        const size_t start = (size_ < cap) ? 0 : head_;
        for (size_t i = 0; i < size_; ++i) {
            fn(buf_[(start + i) % cap]);
        }
    }

    size_t Size()     const noexcept;
    size_t Capacity() const noexcept { return buf_.size(); }
    void   Clear()         noexcept;

private:
    mutable std::mutex        mutex_;
    std::vector<StoredSample> buf_;
    size_t                    head_ = 0;  // next write position
    size_t                    size_ = 0;  // up to buf_.size()
};

} // namespace audio

#endif // AUDIO_ENGINE_TELEMETRY_H
