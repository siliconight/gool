// include/audio_engine/logging.h
//
// Event-level structured logging. Distinct from telemetry (which
// publishes aggregated stats at intervals): logging emits one event
// per occurrence, telling you *why* a counter moved. Same observability
// shape — host-supplied sink, configurable level, structured fields —
// but called per-event rather than per-interval.
//
// THREADING
// =========
// The runtime serializes calls into the sink via an internal mutex,
// so sink implementations do NOT need to be thread-safe themselves.
// Custom sinks should still be cheap (the runtime briefly blocks on
// the sink call), but they can use plain non-atomic state.
//
// Hook points fire from whatever thread the underlying event lives on:
//   * game thread   — late-event discard, RTPC budget exceeded,
//                      one-shot drops/evictions, mixer-underrun delta
//   * network thread — replication validator/policy rejections,
//                      voice rate-limit rejections
//   * render thread  — never logs directly; the game thread observes
//                      the render-thread counter delta in Update step 12
//                      and emits the log line from there.
//
// FAST PATH WHEN DISABLED
// =======================
// `ShouldLog(level)` is one nullptr-check + one uint8 compare on
// members that don't change after Initialize, so disabled categories
// cost a branch, not a sink call. Field-array construction at the
// call site is also skipped when the level is filtered out — see
// the AE_LOG_FIELDS macro for the recommended call shape.

#ifndef AUDIO_ENGINE_LOGGING_H
#define AUDIO_ENGINE_LOGGING_H

#include "audio_engine/types.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace audio {

// Severity of a log event. Filter via AudioConfig::logMinLevel; events
// at a level below the configured minimum are dropped before the
// sink is consulted (and field-array construction at the call site
// skipped via ShouldLog).
enum class LogLevel : uint8_t {
    Trace = 0,  // very high-volume, off by default
    Debug = 1,  // dev-loop diagnostics
    Info  = 2,  // significant lifecycle events
    Warn  = 3,  // unexpected-but-recoverable
    Error = 4,  // unrecoverable for this operation; runtime keeps ticking
};

// v0.79.1: compile-time minimum log level. Calls to ShouldLog_ with a
// statically-known LogLevel below this value constant-fold to false at
// compile time, and the linker drops the associated Log_(...) call sites
// (including their string literals and field-array construction) via
// dead-code elimination.
//
// Default is 1 (Debug) — all runtime log levels remain available so
// hosts can raise verbosity at runtime in shipped builds for player
// bug reports. This preserves v0.78.5's "diagnose your install" /
// "capture session logs from a misbehaving player's machine" workflow.
//
// Opt-in for tighter shipping binaries: pass -DGOOL_LOG_MIN_LEVEL=3
// (Warn) on the compile line. This is NOT set by CMake's Release
// config — the ~5 KB binary savings doesn't justify silently
// removing the ability to do field diagnostics. Hosts that don't
// need the diagnostic capability can opt in explicitly.
//
// Going to 5 would strip everything, including the v0.15.0 Update-
// thread exception reporting — not recommended.
#ifndef GOOL_LOG_MIN_LEVEL
#define GOOL_LOG_MIN_LEVEL 1
#endif

// Stable category names for the runtime's built-in hook points. Hosts
// can introduce their own categories — the runtime's level filter is
// purely level-based in this iteration, not per-category.
namespace LogCategory {
    constexpr std::string_view kEvents      = "events";
    constexpr std::string_view kMixer       = "mixer";
    constexpr std::string_view kVoice       = "voice";
    constexpr std::string_view kRtpc        = "rtpc";
    constexpr std::string_view kEmitter     = "emitter";
    constexpr std::string_view kPrediction  = "prediction";
    constexpr std::string_view kReplication = "replication";
}  // namespace LogCategory

// One structured field on a log event. Values are typed primitives;
// sinks format them however suits their backend. The struct is
// stack-friendly (one tag byte + 16-byte payload) so call sites can
// build a small fields array on the stack without heap traffic.
struct LogField {
    enum class Type : uint8_t { Int64, UInt64, Float, Bool, StrView };

    std::string_view key;
    Type             type;
    union Value {
        int64_t          i64;
        uint64_t         u64;
        double           f64;
        bool             b;
        std::string_view sv;
        Value() : u64(0) {}
    } value;

    // Helper constructors keep call sites tidy:
    //   AE_LOG_FIELDS(rt_, LogLevel::Debug, "events", "late discard",
    //       LogField::UInt("event_ts", e.timestampMs),
    //       LogField::UInt("now_ms", nowMs));
    static LogField Int(std::string_view k, int64_t v) noexcept {
        LogField f; f.key = k; f.type = Type::Int64; f.value.i64 = v; return f;
    }
    static LogField UInt(std::string_view k, uint64_t v) noexcept {
        LogField f; f.key = k; f.type = Type::UInt64; f.value.u64 = v; return f;
    }
    static LogField Float(std::string_view k, double v) noexcept {
        LogField f; f.key = k; f.type = Type::Float; f.value.f64 = v; return f;
    }
    static LogField Bool(std::string_view k, bool v) noexcept {
        LogField f; f.key = k; f.type = Type::Bool; f.value.b = v; return f;
    }
    static LogField Str(std::string_view k, std::string_view v) noexcept {
        LogField f; f.key = k; f.type = Type::StrView; f.value.sv = v; return f;
    }
};

// One log event, passed by const-ref to the sink. The fields span
// is borrowed from the call site and is only valid for the duration
// of the OnLogEvent call. Sinks that store events (e.g. RingLogSink)
// must deep-copy.
struct LogEvent {
    TimestampMs               timestampMs;
    LogLevel                  level;
    std::string_view          category;
    std::string_view          message;
    std::span<const LogField> fields;
};

// Sink interface. Hosts implement this and pass the pointer in
// AudioRuntimeDependencies::logSink. The runtime takes a mutex around
// the call so implementations don't need to be thread-safe themselves.
class IRuntimeLogSink {
public:
    virtual ~IRuntimeLogSink() = default;
    virtual void OnLogEvent(const LogEvent& event) = 0;
};

// =============================================================================
// JSON Lines log sink
// =============================================================================
//
// Writes one JSON object per event, newline-terminated. Compact, no
// pretty-printing. Field key order on the wire is: ts, level, category,
// message, then host-supplied fields in call order. Atomic at the FD
// level for typical line sizes (< PIPE_BUF) so multi-process logs
// don't interleave on POSIX.
//
//     auto sink = std::make_unique<audio::JsonLinesLogSink>(stdout);
//     deps.logSink = sink.get();
//     cfg.logMinLevel = audio::LogLevel::Info;
class JsonLinesLogSink final : public IRuntimeLogSink {
public:
    explicit JsonLinesLogSink(std::FILE* out = stdout) noexcept : out_(out) {}
    void OnLogEvent(const LogEvent& event) override;
private:
    std::FILE* out_ = nullptr;
};

// =============================================================================
// Ring buffer log sink (in-memory event history)
// =============================================================================
//
// Keeps the last N events in memory. Mirrors RingTelemetrySink in
// shape and use cases — debug overlays, post-mortems, replay
// correlation. Each StoredEvent owns its strings and fields (the
// LogEvent on the wire is borrowed; the ring deep-copies on store).
//
// Thread-safe: an internal mutex serializes OnLogEvent() (which the
// runtime can call from game OR network thread depending on hook
// site) against Snapshot() / Size() / Capacity() / Clear() (which
// the host may call from any thread). ForEachInOrder() is
// intentionally NOT locked — it walks stored copies but does so
// without holding the mutex, so callers must guarantee no concurrent
// OnLogEvent. Use Snapshot() if you need to inspect from a
// different thread than the runtime's Update.
class RingLogSink final : public IRuntimeLogSink {
public:
    explicit RingLogSink(size_t capacity = 256) {
        buf_.resize(std::max<size_t>(capacity, 1));
    }

    void OnLogEvent(const LogEvent& event) override;

    // One stored event. Owns its strings and fields so the contents
    // remain valid after the OnLogEvent call returns.
    struct StoredEvent {
        TimestampMs           timestampMs = 0;
        LogLevel              level       = LogLevel::Info;
        std::string           category;
        std::string           message;
        std::vector<LogField> fields;
        // Backing storage for any string fields whose source went out
        // of scope after OnLogEvent returned. Indices in `fields`
        // point at these via re-pointed string_views.
        std::vector<std::string> stringStorage;
    };

    std::vector<StoredEvent> Snapshot() const;

    template <typename F>
    void ForEachInOrder(F&& fn) const {
        if (size_ == 0) return;
        const size_t cap   = buf_.size();
        const size_t start = (size_ < cap) ? 0 : head_;
        for (size_t i = 0; i < size_; ++i) {
            fn(buf_[(start + i) % cap]);
        }
    }

    size_t Size()     const noexcept;
    size_t Capacity() const noexcept { return buf_.size(); }
    void   Clear()         noexcept;

private:
    mutable std::mutex       mutex_;
    std::vector<StoredEvent> buf_;
    size_t                   head_ = 0;
    size_t                   size_ = 0;
};

}  // namespace audio

#endif  // AUDIO_ENGINE_LOGGING_H
