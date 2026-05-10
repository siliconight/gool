// tests/unit/logging_test.cpp
//
// Covers:
//   1. JsonLinesLogSink: each event produces one JSON line with the
//      expected field order and JSON-escaped values.
//   2. RingLogSink: deep-copies events, evicts oldest at capacity,
//      preserves StrView fields after the original buffer goes away.
//   3. AudioConfig::logMinLevel filters events below threshold; events
//      at or above threshold reach the sink.
//   4. Null sink with non-zero log level is safe (fast-path).
//   5. End-to-end hook points fire from real runtime code paths:
//      - Late event discard produces an "events" log line
//      - RTPC budget exceeded produces an "rtpc" log line
//      - Replication policy violation produces a "replication" line
//
// Logging is metadata, not audio — these tests assert on the sink's
// captured events, not on rendered samples.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/logging.h"
#include "audio_engine/types.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

// A capturing sink for white-box tests: stores every event.
class CapturingSink final : public audio::IRuntimeLogSink {
public:
    void OnLogEvent(const audio::LogEvent& e) override {
        Captured c;
        c.timestampMs = e.timestampMs;
        c.level       = e.level;
        c.category    = std::string(e.category);
        c.message     = std::string(e.message);
        for (const auto& f : e.fields) {
            CapturedField cf;
            cf.key = std::string(f.key);
            cf.type = f.type;
            switch (f.type) {
                case audio::LogField::Type::Int64:   cf.i64 = f.value.i64; break;
                case audio::LogField::Type::UInt64:  cf.u64 = f.value.u64; break;
                case audio::LogField::Type::Float:   cf.f64 = f.value.f64; break;
                case audio::LogField::Type::Bool:    cf.b   = f.value.b;   break;
                case audio::LogField::Type::StrView: cf.s   = std::string(f.value.sv); break;
            }
            c.fields.push_back(std::move(cf));
        }
        events.push_back(std::move(c));
    }

    struct CapturedField {
        std::string             key;
        audio::LogField::Type   type;
        int64_t  i64 = 0; uint64_t u64 = 0; double f64 = 0.0;
        bool     b   = false; std::string s;
    };
    struct Captured {
        audio::TimestampMs        timestampMs = 0;
        audio::LogLevel           level       = audio::LogLevel::Info;
        std::string               category;
        std::string               message;
        std::vector<CapturedField> fields;
    };
    std::vector<Captured> events;

    int CountByCategory(std::string_view cat) const {
        int n = 0;
        for (const auto& e : events) if (e.category == cat) ++n;
        return n;
    }
};

void InitRuntimeWithLogSink(audio::AudioRuntime& rt,
                              audio::IRuntimeLogSink* sink,
                              audio::LogLevel minLevel = audio::LogLevel::Debug) {
    audio::AudioConfig cfg;
    cfg.sampleRate    = 48000;
    cfg.bufferSize    = 256;
    cfg.outputMode    = audio::AudioOutputMode::Stereo;
    cfg.logMinLevel   = static_cast<uint8_t>(minLevel);

    audio::AudioRuntimeDependencies deps;
    deps.logSink = sink;
    auto rc = rt.Initialize(cfg, std::move(deps));
    assert(rc == audio::AudioResult::Success);
}

// --- Sink format tests -----------------------------------------------------

void TestJsonLinesProducesCompactLine() {
    std::cout << "  [json lines: one compact JSON object per event]\n";
    char* buf = nullptr;
    size_t bufSize = 0;
    FILE* mem = open_memstream(&buf, &bufSize);
    assert(mem != nullptr);

    audio::JsonLinesLogSink sink(mem);
    const audio::LogField fields[] = {
        audio::LogField::UInt("count", 42),
        audio::LogField::Str ("reason", "test"),
        audio::LogField::Bool("flag",   true),
        audio::LogField::Float("ratio", 0.5),
    };
    audio::LogEvent e{
        1234,
        audio::LogLevel::Warn,
        "test_category",
        "hello world",
        std::span<const audio::LogField>(fields, 4),
    };
    sink.OnLogEvent(e);
    std::fflush(mem);
    const std::string out(buf, bufSize);
    std::fclose(mem);
    std::free(buf);

    std::cout << "    out=" << out;
    // One newline at end.
    assert(!out.empty() && out.back() == '\n');
    int newlines = 0;
    for (char c : out) if (c == '\n') ++newlines;
    assert(newlines == 1);
    // Required keys.
    assert(out.find("\"ts\":1234") != std::string::npos);
    assert(out.find("\"level\":\"warn\"") != std::string::npos);
    assert(out.find("\"category\":\"test_category\"") != std::string::npos);
    assert(out.find("\"message\":\"hello world\"") != std::string::npos);
    assert(out.find("\"count\":42") != std::string::npos);
    assert(out.find("\"reason\":\"test\"") != std::string::npos);
    assert(out.find("\"flag\":true") != std::string::npos);
    assert(out.find("\"ratio\":0.5") != std::string::npos);
}

void TestJsonLinesEscapesSpecialCharacters() {
    std::cout << "  [json lines: special chars escape correctly]\n";
    char* buf = nullptr;
    size_t bufSize = 0;
    FILE* mem = open_memstream(&buf, &bufSize);
    audio::JsonLinesLogSink sink(mem);

    const audio::LogField fields[] = {
        audio::LogField::Str("text", "line1\nline2\t\"quoted\""),
    };
    audio::LogEvent e{
        0, audio::LogLevel::Info, "x", "msg",
        std::span<const audio::LogField>(fields, 1),
    };
    sink.OnLogEvent(e);
    std::fflush(mem);
    const std::string out(buf, bufSize);
    std::fclose(mem); std::free(buf);

    // Tabs, newlines, and quotes all escaped.
    assert(out.find("\\n") != std::string::npos);
    assert(out.find("\\t") != std::string::npos);
    assert(out.find("\\\"quoted\\\"") != std::string::npos);
}

void TestRingLogSinkOrdersEvictsAndDeepCopies() {
    std::cout << "  [ring log: chronological, evicts at capacity, deep-copies StrView]\n";
    audio::RingLogSink ring(/*capacity*/ 3);

    // Build the test strings inside a scope so they go away before
    // we Snapshot — proves deep copy.
    {
        const std::string msg1 = "first";
        const std::string msg2 = "second";
        const std::string msg3 = "third";
        const std::string msg4 = "fourth";

        audio::LogField f1[] = { audio::LogField::Str("k", msg1) };
        audio::LogField f2[] = { audio::LogField::Str("k", msg2) };
        audio::LogField f3[] = { audio::LogField::Str("k", msg3) };
        audio::LogField f4[] = { audio::LogField::Str("k", msg4) };

        ring.OnLogEvent({100, audio::LogLevel::Info, "c", msg1,
                          std::span<const audio::LogField>(f1, 1)});
        ring.OnLogEvent({200, audio::LogLevel::Info, "c", msg2,
                          std::span<const audio::LogField>(f2, 1)});
        ring.OnLogEvent({300, audio::LogLevel::Info, "c", msg3,
                          std::span<const audio::LogField>(f3, 1)});
        ring.OnLogEvent({400, audio::LogLevel::Info, "c", msg4,
                          std::span<const audio::LogField>(f4, 1)});
    }
    // Local strings out of scope. RingLogSink must own copies.

    auto snap = ring.Snapshot();
    assert(snap.size() == 3);
    // Oldest (100) was evicted; we have 200, 300, 400 in order.
    assert(snap[0].timestampMs == 200);
    assert(snap[1].timestampMs == 300);
    assert(snap[2].timestampMs == 400);
    // Message strings preserved.
    assert(snap[0].message == "second");
    assert(snap[2].message == "fourth");
    // String fields preserved (the deep copy worked).
    assert(snap[0].fields.size() == 1);
    assert(snap[0].fields[0].type == audio::LogField::Type::StrView);
    assert(std::string(snap[0].fields[0].value.sv) == "second");
    assert(std::string(snap[2].fields[0].value.sv) == "fourth");
}

// --- Level filtering -------------------------------------------------------

void TestLogLevelFiltersBelow() {
    std::cout << "  [level filter: events below logMinLevel never reach sink]\n";
    CapturingSink sink;
    audio::AudioRuntime rt;
    InitRuntimeWithLogSink(rt, &sink, audio::LogLevel::Warn);

    // Trigger a Debug-level hook point: late event discard. The
    // event is below Warn, so it must not reach the sink. (Note:
    // timestampMs == 0 is treated as "unset" and exempt from the
    // late check, so we use 1 with a tight staleness budget.)
    audio::AudioEvent e = audio::AudioEvent::MakePlaySoundAtLocation(
        0xFEED, audio::Vec3{0,0,0});
    e.timestampMs    = 1;
    e.maxStalenessMs = 1;

    rt.SubmitEvent(e);
    rt.Update(1.0f);  // 1 second elapsed → far past 1ms staleness
    rt.Shutdown();

    assert(sink.events.empty());
    std::cout << "    OK (filter blocked Debug event)\n";
}

void TestLogLevelLetsThroughAtOrAbove() {
    std::cout << "  [level filter: events at or above logMinLevel reach sink]\n";
    CapturingSink sink;
    audio::AudioRuntime rt;
    InitRuntimeWithLogSink(rt, &sink, audio::LogLevel::Debug);

    audio::AudioEvent e = audio::AudioEvent::MakePlaySoundAtLocation(
        0xBEEF, audio::Vec3{0,0,0});
    e.timestampMs    = 1;
    e.maxStalenessMs = 1;

    rt.SubmitEvent(e);
    rt.Update(1.0f);
    rt.Shutdown();

    int eventLogs = sink.CountByCategory("events");
    std::cout << "    sink received " << eventLogs << " 'events' log(s)\n";
    assert(eventLogs >= 1);
}

void TestNullLogSinkIsSafe() {
    std::cout << "  [runtime: null log sink with low logMinLevel is safe]\n";
    audio::AudioRuntime rt;
    InitRuntimeWithLogSink(rt, nullptr, audio::LogLevel::Trace);

    // Force a few late discards and updates. Should not crash.
    audio::AudioEvent e = audio::AudioEvent::MakePlaySoundAtLocation(
        0xCAFE, audio::Vec3{0,0,0});
    e.timestampMs    = 1;
    e.maxStalenessMs = 1;
    for (int i = 0; i < 10; ++i) {
        rt.SubmitEvent(e);
        rt.Update(0.1f);
    }
    rt.Shutdown();
    std::cout << "    OK\n";
}

// --- End-to-end hook point tests ------------------------------------------

void TestRtpcBudgetExceededLogs() {
    std::cout << "  [hook: RTPC budget exceeded fires a Warn log]\n";
    CapturingSink sink;
    audio::AudioRuntime rt;

    audio::AudioConfig cfg;
    cfg.sampleRate            = 48000;
    cfg.bufferSize            = 256;
    cfg.outputMode            = audio::AudioOutputMode::Stereo;
    cfg.logMinLevel           = static_cast<uint8_t>(audio::LogLevel::Debug);
    cfg.maxSoundRtpcBindings  = 1;  // tight cap so we hit it

    audio::AudioRuntimeDependencies deps;
    deps.logSink = &sink;
    auto rc = rt.Initialize(cfg, std::move(deps));
    assert(rc == audio::AudioResult::Success);

    const auto p = audio::HashParameterName("p");
    audio::SoundRtpcBinding b;
    b.paramId = p; b.target = audio::RtpcTarget::Volume;
    b.minValue = 0; b.maxValue = 1; b.minOutput = 0; b.maxOutput = 1;

    // First fits.
    auto r1 = rt.SetSoundRtpc(0xABCD, b);
    assert(r1 == audio::AudioResult::Success);
    // Second exceeds the cap.
    auto r2 = rt.SetSoundRtpc(0xABCE, b);
    assert(r2 == audio::AudioResult::BudgetExceeded);

    rt.Shutdown();

    // Exactly one rtpc log line, level Warn.
    int rtpcLogs = sink.CountByCategory("rtpc");
    std::cout << "    sink received " << rtpcLogs << " 'rtpc' log(s)\n";
    assert(rtpcLogs == 1);
    // Verify the level + a key field.
    bool found = false;
    for (const auto& e : sink.events) {
        if (e.category == "rtpc") {
            assert(e.level == audio::LogLevel::Warn);
            assert(e.message.find("budget exceeded") != std::string::npos);
            for (const auto& f : e.fields) {
                if (f.key == "budget") {
                    assert(f.u64 == 1);
                    found = true;
                }
            }
        }
    }
    assert(found);
}

void TestPolicyViolationLogs() {
    std::cout << "  [hook: replication policy violation fires a Warn log]\n";
    CapturingSink sink;
    audio::AudioRuntime rt;
    InitRuntimeWithLogSink(rt, &sink, audio::LogLevel::Debug);

    // A Client-sourced event marked ServerAuthoritative is a spoof
    // attempt and gets rejected by the protocol-policy enforcement
    // step before any other validation runs.
    audio::AudioEvent e = audio::AudioEvent::MakePlaySoundAtLocation(
        0xFEED, audio::Vec3{0,0,0});
    e.replicationPolicy = audio::AudioReplicationPolicy::ServerAuthoritative;
    e.playerId          = 99;

    auto r = rt.SubmitReplicatedEvent(e, audio::ReplicationSource::Client);
    assert(r == audio::AudioResult::PolicyViolation);

    rt.Shutdown();

    int repLogs = sink.CountByCategory("replication");
    std::cout << "    sink received " << repLogs << " 'replication' log(s)\n";
    assert(repLogs == 1);
    for (const auto& ev : sink.events) {
        if (ev.category == "replication") {
            assert(ev.level == audio::LogLevel::Warn);
            // Must carry player_id field.
            bool gotPid = false;
            for (const auto& f : ev.fields) {
                if (f.key == "player_id") {
                    assert(f.u64 == 99);
                    gotPid = true;
                }
            }
            assert(gotPid);
        }
    }
}

void TestLateEventDiscardLogs() {
    std::cout << "  [hook: late-event discard fires a Debug log]\n";
    CapturingSink sink;
    audio::AudioRuntime rt;
    InitRuntimeWithLogSink(rt, &sink, audio::LogLevel::Debug);

    audio::AudioEvent e = audio::AudioEvent::MakePlaySoundAtLocation(
        0xDEAD, audio::Vec3{0,0,0});
    e.timestampMs    = 1;
    e.maxStalenessMs = 1;

    rt.SubmitEvent(e);
    rt.Update(1.0f);  // far past staleness
    rt.Shutdown();

    int evLogs = sink.CountByCategory("events");
    std::cout << "    sink received " << evLogs << " 'events' log(s)\n";
    assert(evLogs == 1);
    for (const auto& ev : sink.events) {
        if (ev.category == "events") {
            assert(ev.level == audio::LogLevel::Debug);
            assert(ev.message.find("late") != std::string::npos);
            // Carries replicated field.
            bool gotRep = false;
            for (const auto& f : ev.fields) {
                if (f.key == "replicated") {
                    assert(f.b == false);  // SubmitEvent is local
                    gotRep = true;
                }
            }
            assert(gotRep);
        }
    }
}

}  // namespace

int main() {
    std::cout << "[logging_test]\n";
    TestJsonLinesProducesCompactLine();
    TestJsonLinesEscapesSpecialCharacters();
    TestRingLogSinkOrdersEvictsAndDeepCopies();
    TestLogLevelFiltersBelow();
    TestLogLevelLetsThroughAtOrAbove();
    TestNullLogSinkIsSafe();
    TestRtpcBudgetExceededLogs();
    TestPolicyViolationLogs();
    TestLateEventDiscardLogs();
    std::cout << "[logging_test] PASSED\n";
    return 0;
}
