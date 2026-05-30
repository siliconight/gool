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

// src/audio_engine/runtime/telemetry.cpp
//
// Implementations of the three built-in telemetry sinks. Each one is
// independent — including only audio_engine/telemetry.h is enough to
// link, no internal-impl headers required.

#include "audio_engine/telemetry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace audio {

// ---------------------------------------------------------------------------
// JsonLinesTelemetrySink
// ---------------------------------------------------------------------------
//
// Writes one JSON object per stats sample, newline-terminated. Compact
// (no pretty-printing), deterministic field order, every field always
// emitted. Uses fprintf rather than ostream to avoid pulling in
// iostream's allocations and locale machinery on the hot path.

void JsonLinesTelemetrySink::OnRuntimeStats(const RuntimeStatsSample& sample) {
    if (out_ == nullptr) return;

    const auto& s = sample.stats;
    // Single fprintf — atomic at the FD level for typical line sizes
    // (<4 KB), so multi-process logs don't interleave. Field order is
    // stable for downstream schema parsers. Counters use %llu, gauges
    // use %u — the cast is safe given the struct's uint{32,64}_t types.
    (void)std::fprintf(out_,
        "{"
        "\"ts\":%llu,"
        "\"active_emitters\":%u,"
        "\"active_voice_sources\":%u,"
        "\"events_drained\":%u,"
        "\"late_events_discarded\":%u,"
        "\"occlusion_checks\":%u,"
        "\"mixer_voices_active\":%u,"
        "\"render_underruns\":%llu,"
        "\"total_render_callbacks\":%llu,"
        "\"oneshots_dropped_full_pool\":%llu,"
        "\"oneshot_evictions\":%llu,"
        "\"predictions_cancelled\":%llu,"
        "\"predictions_cancelled_not_found\":%llu,"
        "\"emitters_processed\":%u,"
        "\"emitters_skipped_by_interest\":%u,"
        "\"replication_rate_limited_sfx\":%llu,"
        "\"replication_rate_limited_voice\":%llu,"
        "\"replication_rate_limited_music\":%llu,"
        "\"replication_rate_limited_ambience\":%llu,"
        "\"replication_rate_limited_ui\":%llu,"
        "\"replication_rate_limited_dialogue\":%llu,"
        "\"replication_rejected_validator\":%llu,"
        "\"replication_policy_violations\":%llu,"
        "\"replication_rejected_new_id_budget\":%llu,"
        "\"voice_frames_dropped_due_to_mute\":%llu,"
        "\"voice_bytes_sent\":%llu,"
        "\"voice_frames_budget_downgraded\":%llu,"
        "\"voice_frames_budget_dropped\":%llu"
        "}\n",
        static_cast<unsigned long long>(sample.timestampMs),
        s.activeEmitters,
        s.activeVoiceSources,
        s.eventsDrainedLastTick,
        s.lateEventsDiscardedLastTick,
        s.occlusionChecksLastTick,
        s.mixerVoicesActive,
        static_cast<unsigned long long>(s.renderUnderruns),
        static_cast<unsigned long long>(s.totalRenderCallbacks),
        static_cast<unsigned long long>(s.oneShotsDroppedFullPool),
        static_cast<unsigned long long>(s.oneShotEvictions),
        static_cast<unsigned long long>(s.predictionsCancelled),
        static_cast<unsigned long long>(s.predictionsCancelledNotFound),
        s.emittersProcessedLastTick,
        s.emittersSkippedByInterestLastTick,
        static_cast<unsigned long long>(s.replicationEventsRateLimited[0]),
        static_cast<unsigned long long>(s.replicationEventsRateLimited[1]),
        static_cast<unsigned long long>(s.replicationEventsRateLimited[2]),
        static_cast<unsigned long long>(s.replicationEventsRateLimited[3]),
        static_cast<unsigned long long>(s.replicationEventsRateLimited[4]),
        static_cast<unsigned long long>(s.replicationEventsRateLimited[5]),
        static_cast<unsigned long long>(s.replicationEventsRejectedByValidator),
        static_cast<unsigned long long>(s.replicationPolicyViolations),
        static_cast<unsigned long long>(s.replicationEventsRejectedNewIdBudget),
        static_cast<unsigned long long>(s.voiceFramesDroppedDueToMute),
        static_cast<unsigned long long>(s.voiceBytesSent),
        static_cast<unsigned long long>(s.voiceFramesBudgetDowngraded),
        static_cast<unsigned long long>(s.voiceFramesBudgetDropped));
}

// ---------------------------------------------------------------------------
// PrometheusTelemetrySink
// ---------------------------------------------------------------------------
//
// Builds an exposition-format string in scratch storage, then atomically
// swaps it under the mutex. The HTTP scrape handler pulls a copy via
// Snapshot(). This avoids holding the lock for the duration of string
// formatting, which would block scrapes during metric updates.
//
// Naming follows Prometheus conventions:
//   * snake_case
//   * `gool_` prefix for the namespace
//   * `_total` suffix on monotonic counters
//   * gauges have no suffix
//
// Each metric block emits HELP and TYPE lines followed by the value.
// HELP text describes the metric's meaning so dashboard authors don't
// have to guess; TYPE classifies counter vs gauge so PromQL knows
// whether `rate()` makes sense.

namespace {

// Format a counter (monotonic, _total suffix).
void AppendCounter(std::string& out,
                    const std::string& job,
                    const char* name,
                    const char* help,
                    unsigned long long value) {
    char buf[256];
    int n = std::snprintf(buf, sizeof(buf),
        "# HELP gool_%s %s\n"
        "# TYPE gool_%s counter\n"
        "gool_%s{job=\"%s\"} %llu\n",
        name, help, name, name, job.c_str(), value);
    if (n > 0) {
        // snprintf returns the would-have-been length, not the
        // actually-written length. If the format string overflowed
        // `buf`, n > sizeof(buf) and `out.append(buf, n)` would read
        // past the end of the buffer (stack-buffer-overflow caught by
        // ASAN). Clamp to the actually-written length (sizeof(buf)-1
        // accounting for the implicit null terminator).
        const size_t toCopy =
            std::min(static_cast<size_t>(n), sizeof(buf) - 1);
        out.append(buf, toCopy);
    }
}
void AppendGauge(std::string& out,
                  const std::string& job,
                  const char* name,
                  const char* help,
                  unsigned long long value) {
    char buf[256];
    int n = std::snprintf(buf, sizeof(buf),
        "# HELP gool_%s %s\n"
        "# TYPE gool_%s gauge\n"
        "gool_%s{job=\"%s\"} %llu\n",
        name, help, name, name, job.c_str(), value);
    if (n > 0) {
        // Same clamping rule as AppendCounter above — see comment there.
        const size_t toCopy =
            std::min(static_cast<size_t>(n), sizeof(buf) - 1);
        out.append(buf, toCopy);
    }
}

// Counter with a category label (used for the per-category replication
// rate-limit array). `category` is an exposition-format label value;
// caller must ensure it's already escape-safe (no quotes, no newlines).
void AppendCategoryCounter(std::string& out,
                            const std::string& job,
                            const char* name,
                            const char* help,
                            const char* category,
                            unsigned long long value) {
    char buf[320];
    int n = std::snprintf(buf, sizeof(buf),
        "# HELP gool_%s %s\n"
        "# TYPE gool_%s counter\n"
        "gool_%s{job=\"%s\",category=\"%s\"} %llu\n",
        name, help, name, name, job.c_str(), category, value);
    if (n > 0) {
        // Same clamping rule as AppendCounter above — see comment there.
        const size_t toCopy =
            std::min(static_cast<size_t>(n), sizeof(buf) - 1);
        out.append(buf, toCopy);
    }
}

} // namespace

void PrometheusTelemetrySink::OnRuntimeStats(const RuntimeStatsSample& sample) {
    const auto& s = sample.stats;

    // Build into a scratch string, then swap under the mutex. Avoids
    // holding the lock during 24 string concatenations.
    std::string fresh;
    fresh.reserve(4096);  // typical exposition payload

    // ---- Gauges (point-in-time values) ----
    AppendGauge(fresh, job_, "active_emitters",
                "Number of currently active 3D audio emitters.",
                s.activeEmitters);
    AppendGauge(fresh, job_, "active_voice_sources",
                "Number of currently registered per-player voice sources.",
                s.activeVoiceSources);
    AppendGauge(fresh, job_, "events_drained_last_tick",
                "Audio events processed in the last Update tick.",
                s.eventsDrainedLastTick);
    AppendGauge(fresh, job_, "late_events_discarded_last_tick",
                "Audio events dropped last tick due to late arrival.",
                s.lateEventsDiscardedLastTick);
    AppendGauge(fresh, job_, "occlusion_checks_last_tick",
                "Occlusion raycasts performed in the last Update tick.",
                s.occlusionChecksLastTick);
    AppendGauge(fresh, job_, "mixer_voices_active",
                "Number of voice slots actively rendering audio in the mixer.",
                s.mixerVoicesActive);
    AppendGauge(fresh, job_, "emitters_processed_last_tick",
                "Spatialized emitters fully processed by the orchestrator last tick.",
                s.emittersProcessedLastTick);
    AppendGauge(fresh, job_, "emitters_skipped_by_interest_last_tick",
                "Emitters skipped last tick because the interest-management budget was exhausted.",
                s.emittersSkippedByInterestLastTick);

    // ---- Counters (monotonic) ----
    AppendCounter(fresh, job_, "render_underruns_total",
                   "Render-thread callback budget overruns since runtime init.",
                   static_cast<unsigned long long>(s.renderUnderruns));
    AppendCounter(fresh, job_, "render_callbacks_total",
                   "Total render-thread callbacks served since runtime init.",
                   static_cast<unsigned long long>(s.totalRenderCallbacks));
    AppendCounter(fresh, job_, "oneshots_dropped_full_pool_total",
                   "One-shot sounds dropped because the emitter pool was full of higher-priority entries.",
                   static_cast<unsigned long long>(s.oneShotsDroppedFullPool));
    AppendCounter(fresh, job_, "oneshot_evictions_total",
                   "One-shots that evicted lower-priority playing entries to claim a slot.",
                   static_cast<unsigned long long>(s.oneShotEvictions));
    AppendCounter(fresh, job_, "predictions_cancelled_total",
                   "Predicted events successfully cancelled by reconciliation.",
                   static_cast<unsigned long long>(s.predictionsCancelled));
    AppendCounter(fresh, job_, "predictions_cancelled_not_found_total",
                   "Prediction-cancellations that didn't match an active emitter.",
                   static_cast<unsigned long long>(s.predictionsCancelledNotFound));
    AppendCounter(fresh, job_, "replication_rejected_validator_total",
                   "Replicated events rejected by the host's IReplicationValidator hook.",
                   static_cast<unsigned long long>(s.replicationEventsRejectedByValidator));
    AppendCounter(fresh, job_, "replication_policy_violations_total",
                   "Replicated events rejected for protocol policy violations (Phase 2.5).",
                   static_cast<unsigned long long>(s.replicationPolicyViolations));
    AppendCounter(fresh, job_, "replication_rejected_new_id_budget_total",
                   "Replicated events rejected because the per-tick new-player admission cap was hit.",
                   static_cast<unsigned long long>(s.replicationEventsRejectedNewIdBudget));

    // ---- Voice mute/budget counters (Phase 2.4 + 2.6) ----
    AppendCounter(fresh, job_, "voice_frames_dropped_due_to_mute_total",
                   "Voice frames dropped at decode boundary because their source was muted.",
                   static_cast<unsigned long long>(s.voiceFramesDroppedDueToMute));
    AppendCounter(fresh, job_, "voice_bytes_sent_total",
                   "Total upstream voice bytes reported by the host via ReportVoiceBytesSent.",
                   static_cast<unsigned long long>(s.voiceBytesSent));
    AppendCounter(fresh, job_, "voice_frames_budget_downgraded_total",
                   "Voice frames the host encoded at a downgraded bitrate due to bandwidth budget pressure.",
                   static_cast<unsigned long long>(s.voiceFramesBudgetDowngraded));
    AppendCounter(fresh, job_, "voice_frames_budget_dropped_total",
                   "Voice frames the host dropped because SuggestVoiceBitrate returned 0 (budget exhausted).",
                   static_cast<unsigned long long>(s.voiceFramesBudgetDropped));

    // ---- Per-category replication rate-limit counter ----
    static constexpr const char* kCategoryNames[6] = {
        "sfx", "voice", "music", "ambience", "ui", "dialogue",
    };
    for (size_t i = 0; i < 6; ++i) {
        AppendCategoryCounter(fresh, job_, "replication_rate_limited_total",
            "Replicated events rejected by the per-category token-bucket rate limiter.",
            kCategoryNames[i],
            static_cast<unsigned long long>(s.replicationEventsRateLimited[i]));
    }

    // Atomic swap.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.swap(fresh);
    }
}

std::string PrometheusTelemetrySink::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

// ---------------------------------------------------------------------------
// RingTelemetrySink
// ---------------------------------------------------------------------------
//
// Classic circular buffer. Capacity is fixed at construction. Write
// position advances on every sample; size grows up to capacity, then
// stays there (oldest sample overwritten). Snapshot walks from the
// oldest to the newest. An internal mutex serializes writes and
// Snapshot() reads so the host can call Snapshot() from a different
// thread than the one driving Update.

void RingTelemetrySink::OnRuntimeStats(const RuntimeStatsSample& sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    StoredSample& slot = buf_[head_];
    slot.timestampMs = sample.timestampMs;
    // Stats is trivially copyable — just memcpy. Saves a constructor
    // roundtrip on the hot path; layout is guaranteed by the
    // standard-layout struct definition in audio_runtime.h.
    std::memcpy(&slot.stats, &sample.stats, sizeof(AudioRuntime::Stats));
    head_ = (head_ + 1) % buf_.size();
    if (size_ < buf_.size()) ++size_;
}

std::vector<RingTelemetrySink::StoredSample>
RingTelemetrySink::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StoredSample> out;
    out.reserve(size_);
    if (size_ != 0) {
        const size_t cap   = buf_.size();
        const size_t start = (size_ < cap) ? 0 : head_;
        for (size_t i = 0; i < size_; ++i) {
            out.push_back(buf_[(start + i) % cap]);
        }
    }
    return out;
}

size_t RingTelemetrySink::Size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

void RingTelemetrySink::Clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    size_ = 0;
}

} // namespace audio
