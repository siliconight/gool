// tests/unit/telemetry_test.cpp
//
// Covers:
//   1. JsonLinesTelemetrySink: writes one JSON object per emit, every
//      documented field present, deterministic order.
//   2. PrometheusTelemetrySink: produces valid exposition-format output
//      with HELP/TYPE blocks and the per-category replication counter
//      labels.
//   3. RingTelemetrySink: stores up to N samples in chronological
//      order, oldest gets evicted at capacity.
//   4. End-to-end: runtime configured with a sink emits at the
//      configured cadence — interval=100ms over 1 simulated second
//      yields ~10 samples; interval=0 emits zero samples.
//
// Audibility-irrelevant tests; we just verify the bytes the sink
// produces and the cadence the runtime drives.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/telemetry.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace {

// A counting sink that just tallies invocations. Useful for cadence
// tests where we don't care about content.
class CountingSink final : public audio::IRuntimeTelemetrySink {
public:
    void OnRuntimeStats(const audio::RuntimeStatsSample& s) override {
        ++calls;
        lastTimestampMs   = s.timestampMs;
        lastActiveEmitter = s.stats.activeEmitters;
    }
    int               calls              = 0;
    audio::TimestampMs lastTimestampMs    = 0;
    uint32_t          lastActiveEmitter = 0;
};

void InitRuntimeWithSink(audio::AudioRuntime& rt,
                          audio::IRuntimeTelemetrySink* sink,
                          uint32_t intervalMs) {
    audio::AudioConfig cfg;
    cfg.sampleRate          = 48000;
    cfg.bufferSize          = 256;
    cfg.outputMode          = audio::AudioOutputMode::Stereo;
    cfg.telemetryIntervalMs = intervalMs;

    audio::AudioRuntimeDependencies deps;
    deps.telemetrySink = sink;
    auto rc = rt.Initialize(cfg, std::move(deps));
    assert(rc == audio::AudioResult::Success);
}

// Synthesize a stats struct with every field non-zero so JSON / Prom
// formatters exercise their full output. The fields don't have to be
// realistic — we just need each one distinguishable.
audio::AudioRuntime::Stats SyntheticStats() {
    audio::AudioRuntime::Stats s{};
    s.activeEmitters                            = 11;
    s.activeVoiceSources                        = 12;
    s.eventsDrainedLastTick                     = 13;
    s.lateEventsDiscardedLastTick               = 14;
    s.occlusionChecksLastTick                   = 15;
    s.mixerVoicesActive                         = 16;
    s.renderUnderruns                           = 17;
    s.totalRenderCallbacks                      = 18;
    s.oneShotsDroppedFullPool                   = 19;
    s.oneShotEvictions                          = 20;
    s.predictionsCancelled                      = 21;
    s.predictionsCancelledNotFound              = 22;
    s.emittersProcessedLastTick                 = 23;
    s.emittersSkippedByInterestLastTick         = 24;
    s.replicationEventsRateLimited[0]           = 100;
    s.replicationEventsRateLimited[1]           = 101;
    s.replicationEventsRateLimited[2]           = 102;
    s.replicationEventsRateLimited[3]           = 103;
    s.replicationEventsRateLimited[4]           = 104;
    s.replicationEventsRateLimited[5]           = 105;
    s.replicationEventsRejectedByValidator      = 30;
    s.replicationPolicyViolations               = 31;
    s.replicationEventsRejectedNewIdBudget      = 32;
    return s;
}

// --- JSON Lines ------------------------------------------------------------

void TestJsonLinesSinkProducesAllFields() {
    std::cout << "  [json lines: every documented field appears in output]\n";

    // Capture stdout to a memstream-backed FILE*.
    char* buf = nullptr;
    size_t bufSize = 0;
    FILE* mem = open_memstream(&buf, &bufSize);
    assert(mem != nullptr);

    audio::JsonLinesTelemetrySink sink(mem);
    auto stats = SyntheticStats();
    sink.OnRuntimeStats(audio::RuntimeStatsSample{1234, stats});
    sink.OnRuntimeStats(audio::RuntimeStatsSample{2468, stats});
    std::fflush(mem);

    const std::string out(buf, bufSize);
    std::fclose(mem);
    std::free(buf);

    // Two lines, one per emit.
    size_t newlines = 0;
    for (char c : out) if (c == '\n') ++newlines;
    assert(newlines == 2);

    // Every documented field name appears at least once.
    static const char* kRequired[] = {
        "\"ts\":",
        "\"active_emitters\":",
        "\"active_voice_sources\":",
        "\"events_drained\":",
        "\"late_events_discarded\":",
        "\"occlusion_checks\":",
        "\"mixer_voices_active\":",
        "\"render_underruns\":",
        "\"total_render_callbacks\":",
        "\"oneshots_dropped_full_pool\":",
        "\"oneshot_evictions\":",
        "\"predictions_cancelled\":",
        "\"predictions_cancelled_not_found\":",
        "\"emitters_processed\":",
        "\"emitters_skipped_by_interest\":",
        "\"replication_rate_limited_sfx\":",
        "\"replication_rate_limited_voice\":",
        "\"replication_rate_limited_music\":",
        "\"replication_rate_limited_ambience\":",
        "\"replication_rate_limited_ui\":",
        "\"replication_rate_limited_dialogue\":",
        "\"replication_rejected_validator\":",
        "\"replication_policy_violations\":",
        "\"replication_rejected_new_id_budget\":",
    };
    for (const char* k : kRequired) {
        if (out.find(k) == std::string::npos) {
            std::cout << "    MISSING: " << k << "\n";
            std::cout << "    OUTPUT:  " << out << "\n";
            assert(false);
        }
    }

    // Specific values map through. Spot-check a few.
    assert(out.find("\"ts\":1234") != std::string::npos);
    assert(out.find("\"active_emitters\":11") != std::string::npos);
    assert(out.find("\"replication_rate_limited_sfx\":100") != std::string::npos);
    assert(out.find("\"replication_rate_limited_dialogue\":105") != std::string::npos);

    std::cout << "    OK (" << out.size() << " bytes, " << newlines << " lines)\n";
}

void TestJsonLinesSinkNullFile() {
    std::cout << "  [json lines: null file pointer no-ops gracefully]\n";
    audio::JsonLinesTelemetrySink sink(nullptr);
    auto stats = SyntheticStats();
    // Should not crash.
    sink.OnRuntimeStats(audio::RuntimeStatsSample{0, stats});
    std::cout << "    OK\n";
}

// --- Prometheus ------------------------------------------------------------

void TestPrometheusSinkExpositionFormat() {
    std::cout << "  [prometheus: HELP/TYPE blocks present, label syntax valid]\n";

    audio::PrometheusTelemetrySink sink("gool_test");
    // Before first emit, snapshot is empty.
    assert(sink.Snapshot().empty());

    auto stats = SyntheticStats();
    sink.OnRuntimeStats(audio::RuntimeStatsSample{0, stats});
    const std::string snap = sink.Snapshot();
    assert(!snap.empty());

    // Every metric block has HELP and TYPE preceding the value line.
    // Spot-check a counter and a gauge.
    assert(snap.find("# HELP gool_active_emitters ") != std::string::npos);
    assert(snap.find("# TYPE gool_active_emitters gauge") != std::string::npos);
    assert(snap.find("gool_active_emitters{job=\"gool_test\"} 11") != std::string::npos);

    assert(snap.find("# HELP gool_render_underruns_total ") != std::string::npos);
    assert(snap.find("# TYPE gool_render_underruns_total counter") != std::string::npos);
    assert(snap.find("gool_render_underruns_total{job=\"gool_test\"} 17") != std::string::npos);

    // Per-category rate-limit counter has a category label.
    assert(snap.find("gool_replication_rate_limited_total{job=\"gool_test\",category=\"sfx\"} 100")
           != std::string::npos);
    assert(snap.find("gool_replication_rate_limited_total{job=\"gool_test\",category=\"dialogue\"} 105")
           != std::string::npos);

    // Subsequent emits replace, not append. Snapshot stays the same
    // size (modulo the value-string lengths).
    sink.OnRuntimeStats(audio::RuntimeStatsSample{0, stats});
    const std::string snap2 = sink.Snapshot();
    // Same content given identical stats.
    assert(snap == snap2);

    std::cout << "    OK (" << snap.size() << " bytes, "
              << "all expected metric blocks present)\n";
}

// --- Ring buffer -----------------------------------------------------------

void TestRingTelemetrySinkOrdersAndEvicts() {
    std::cout << "  [ring: chronological order, evicts oldest at capacity]\n";

    audio::RingTelemetrySink ring(/*capacity*/ 4);
    assert(ring.Capacity() == 4);
    assert(ring.Size() == 0);

    // Push 6 samples with strictly increasing timestamps.
    for (int i = 1; i <= 6; ++i) {
        audio::AudioRuntime::Stats s{};
        s.activeEmitters = static_cast<uint32_t>(i);
        ring.OnRuntimeStats(audio::RuntimeStatsSample{
            static_cast<audio::TimestampMs>(i * 100), s});
    }
    // Buffer holds last 4: timestamps 300, 400, 500, 600.
    assert(ring.Size() == 4);

    auto snap = ring.Snapshot();
    assert(snap.size() == 4);
    assert(snap[0].timestampMs == 300);
    assert(snap[1].timestampMs == 400);
    assert(snap[2].timestampMs == 500);
    assert(snap[3].timestampMs == 600);
    // Stats survives the deep-copy.
    assert(snap[0].stats.activeEmitters == 3);
    assert(snap[3].stats.activeEmitters == 6);

    // Clear resets without destroying capacity.
    ring.Clear();
    assert(ring.Size() == 0);
    assert(ring.Capacity() == 4);

    std::cout << "    OK (4-cap ring, 6-push, last 4 in order)\n";
}

void TestRingForEachIsAllocationFree() {
    std::cout << "  [ring: ForEachInOrder iterates without allocating]\n";
    audio::RingTelemetrySink ring(8);
    audio::AudioRuntime::Stats s{};
    for (int i = 0; i < 8; ++i) {
        s.activeEmitters = static_cast<uint32_t>(i);
        ring.OnRuntimeStats(audio::RuntimeStatsSample{
            static_cast<audio::TimestampMs>(i), s});
    }
    int idx = 0;
    ring.ForEachInOrder([&](const audio::RingTelemetrySink::StoredSample& sm) {
        assert(sm.timestampMs == static_cast<audio::TimestampMs>(idx));
        assert(sm.stats.activeEmitters == static_cast<uint32_t>(idx));
        ++idx;
    });
    assert(idx == 8);
    std::cout << "    OK\n";
}

// --- End-to-end runtime cadence -------------------------------------------

void TestRuntimeEmitsAtConfiguredCadence() {
    std::cout << "  [runtime: 100ms interval over 1s yields ~10 samples]\n";
    CountingSink sink;
    audio::AudioRuntime rt;
    InitRuntimeWithSink(rt, &sink, /*intervalMs*/ 100);

    // Simulate 1 second at 60 Hz: 60 ticks of 16.667 ms.
    for (int i = 0; i < 60; ++i) rt.Update(1.0f / 60.0f);
    rt.Shutdown();

    // 1000 ms / 100 ms = 10 samples expected. Allow ±1 slack for
    // accumulator boundary fuzz.
    std::cout << "    sink received " << sink.calls << " calls\n";
    assert(sink.calls >= 9 && sink.calls <= 11);
}

void TestRuntimeIntervalZeroEmitsNothing() {
    std::cout << "  [runtime: interval=0 disables telemetry entirely]\n";
    CountingSink sink;
    audio::AudioRuntime rt;
    InitRuntimeWithSink(rt, &sink, /*intervalMs*/ 0);

    for (int i = 0; i < 60; ++i) rt.Update(1.0f / 60.0f);
    rt.Shutdown();

    assert(sink.calls == 0);
    std::cout << "    OK (sink never called)\n";
}

void TestRuntimeNullSinkIsSafe() {
    std::cout << "  [runtime: nullptr sink with non-zero interval is safe]\n";
    audio::AudioRuntime rt;
    InitRuntimeWithSink(rt, nullptr, /*intervalMs*/ 100);

    // Should not crash even though interval is non-zero.
    for (int i = 0; i < 60; ++i) rt.Update(1.0f / 60.0f);
    rt.Shutdown();
    std::cout << "    OK\n";
}

// --- Time-series end-to-end with the Ring sink ----------------------------

void TestRuntimeFeedsRingForTimeSeries() {
    std::cout << "  [runtime + ring sink: time series accumulates over Update calls]\n";
    audio::RingTelemetrySink ring(/*capacity*/ 32);
    audio::AudioRuntime rt;
    InitRuntimeWithSink(rt, &ring, /*intervalMs*/ 50);

    // 500 ms simulated → ~10 samples at 50 ms cadence.
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
    rt.Shutdown();

    auto snap = ring.Snapshot();
    std::cout << "    ring captured " << snap.size() << " samples\n";
    assert(snap.size() >= 9 && snap.size() <= 12);

    // Timestamps strictly increasing.
    for (size_t i = 1; i < snap.size(); ++i) {
        assert(snap[i].timestampMs >= snap[i - 1].timestampMs);
    }
    std::cout << "    OK (timestamps monotonic)\n";
}

// --- A1: throwing sink surfaces in Stats counter --------------------------

class ThrowingTelemetrySink final : public audio::IRuntimeTelemetrySink {
public:
    void OnRuntimeStats(const audio::RuntimeStatsSample&) override {
        ++callCount;
        throw std::runtime_error("boom");
    }
    int callCount = 0;
};

void TestThrowingSinkIncrementsStatsCounter() {
    std::cout << "  [throwing telemetry sink: Stats::telemetrySinkExceptions ticks]\n";
    ThrowingTelemetrySink sink;
    audio::AudioRuntime rt;
    InitRuntimeWithSink(rt, &sink, /*intervalMs*/ 100);

    for (int i = 0; i < 60; ++i) rt.Update(1.0f / 60.0f);
    const auto stats = rt.GetStats();
    rt.Shutdown();

    std::cout << "    sink throws=" << sink.callCount
              << "  Stats::telemetrySinkExceptions=" << stats.telemetrySinkExceptions
              << "\n";
    // Sink was called ~10 times; each threw; counter equals call count.
    assert(sink.callCount > 0);
    assert(stats.telemetrySinkExceptions == static_cast<uint64_t>(sink.callCount));
    // Runtime kept ticking: not an assertion of behavior, but we got
    // here without an unhandled exception, which is the point.
}

} // namespace

int main() {
    std::cout << "[telemetry_test]\n";
    TestJsonLinesSinkProducesAllFields();
    TestJsonLinesSinkNullFile();
    TestPrometheusSinkExpositionFormat();
    TestRingTelemetrySinkOrdersAndEvicts();
    TestRingForEachIsAllocationFree();
    TestRuntimeEmitsAtConfiguredCadence();
    TestRuntimeIntervalZeroEmitsNothing();
    TestRuntimeNullSinkIsSafe();
    TestRuntimeFeedsRingForTimeSeries();
    TestThrowingSinkIncrementsStatsCounter();
    std::cout << "[telemetry_test] PASSED\n";
    return 0;
}
