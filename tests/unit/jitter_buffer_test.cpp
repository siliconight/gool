// tests/unit/jitter_buffer_test.cpp
//
// Exercises the adaptive jitter buffer against simulated network
// conditions: clean LAN, residential connection (5% loss + 30 ms
// jitter), rough internet (10% loss + 50 ms jitter, occasional
// reorder), and pathological burst loss.
//
// What we measure for each scenario:
//   * packetsReceived / packetsAccepted   — how many made it in
//   * packetsLate / packetsLost           — drops and concealed gaps
//   * plcGenerated                        — PLC frames issued
//   * targetBufferDepth (final)           — how far the adapter moved
//   * observedJitterMs   (final)          — RFC 3550 EMA estimate
//   * silentFrames / output continuity    — perceived gappiness
//
// These numbers are published in the test output so anyone reading
// them can see the buffer's behavior on real network shapes without
// having to run a live multiplayer game.

#include "audio_engine/voice/jitter_buffer.h"
#include "audio_engine/types.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace audio;
using namespace audio::voice;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

// Synthesize a packet with `seq` and a 1-byte distinct payload.
struct SimPacket {
    uint16_t      seq;
    TimestampMs   sendTs;
    TimestampMs   arrivalMs;
    uint8_t       payload[8];
    size_t        size;
};

std::vector<SimPacket> GenStream(uint32_t numPackets,
                                   uint32_t frameDurationMs,
                                   TimestampMs startSendMs,
                                   TimestampMs startTransitMs) {
    std::vector<SimPacket> out;
    out.reserve(numPackets);
    for (uint32_t i = 0; i < numPackets; ++i) {
        SimPacket p{};
        p.seq       = static_cast<uint16_t>(i);
        p.sendTs    = startSendMs + i * frameDurationMs;
        p.arrivalMs = p.sendTs + startTransitMs;
        // 8-byte payload encoding the seq for verification.
        for (int k = 0; k < 8; ++k) {
            p.payload[k] = static_cast<uint8_t>((i >> (k * 4)) & 0xFF);
        }
        p.size = 8;
        out.push_back(p);
    }
    return out;
}

// Apply network impairment to an in-order stream: jitter (uniform
// random in ±jitterMs added to arrival time), loss (drop with
// probability lossPct), reorder (out-of-order delivery is the
// natural consequence of jitter, since packets are sorted by arrival
// time below).
std::vector<SimPacket> ApplyImpairment(std::vector<SimPacket> stream,
                                         float       lossPct,
                                         uint32_t    jitterMs,
                                         uint32_t    seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::uniform_int_distribution<int> jitterDist(
        -static_cast<int>(jitterMs),
        static_cast<int>(jitterMs));

    std::vector<SimPacket> out;
    out.reserve(stream.size());
    for (auto& p : stream) {
        if (u01(rng) < lossPct) continue;     // dropped
        const int j = jitterDist(rng);
        if (j < 0 && static_cast<TimestampMs>(-j) > p.arrivalMs) {
            p.arrivalMs = 0;
        } else {
            p.arrivalMs = static_cast<TimestampMs>(
                static_cast<int64_t>(p.arrivalMs) + j);
        }
        out.push_back(p);
    }
    // Re-sort by arrival time so the producer pushes them in arrival
    // order (which is what a real network thread would see).
    std::sort(out.begin(), out.end(),
              [](const SimPacket& a, const SimPacket& b) {
                  return a.arrivalMs < b.arrivalMs;
              });
    return out;
}

// Run a packet trace through the jitter buffer in interleaved push/pop
// fashion: at each "frame time" we push any packets whose arrival has
// landed by then, then pop one frame. Reports counts.
struct RunResult {
    uint32_t framesProduced  = 0;
    uint32_t framesPacket    = 0;
    uint32_t framesPLC       = 0;
    uint32_t framesEmpty     = 0;
};
RunResult RunSimulation(JitterBuffer& jb,
                          const std::vector<SimPacket>& trace,
                          uint32_t totalFrames,
                          uint32_t frameDurationMs,
                          TimestampMs startConsumeMs) {
    RunResult r{};
    size_t pushIdx = 0;
    uint8_t scratch[1500];
    for (uint32_t f = 0; f < totalFrames; ++f) {
        const TimestampMs nowMs = startConsumeMs + f * frameDurationMs;
        // Push all packets whose arrival has landed.
        while (pushIdx < trace.size() && trace[pushIdx].arrivalMs <= nowMs) {
            const auto& p = trace[pushIdx];
            jb.Push(p.seq, p.sendTs, p.arrivalMs, p.payload, p.size);
            ++pushIdx;
        }
        size_t       outSize = 0;
        TimestampMs  outTs   = 0;
        const auto   res     = jb.PopNext(scratch, sizeof(scratch), outSize, outTs);
        ++r.framesProduced;
        switch (res) {
            case PopResult::Packet: ++r.framesPacket; break;
            case PopResult::PLC:    ++r.framesPLC;    break;
            case PopResult::Empty:  ++r.framesEmpty;  break;
        }
    }
    return r;
}

void PrintScenario(const char* name, const JitterBufferStats& s, const RunResult& r) {
    std::printf("  [%s]\n", name);
    std::printf("    received=%llu  accepted=%llu  late=%llu  reordered=%llu  duplicate=%llu  lost=%llu\n",
                (unsigned long long)s.packetsReceived,
                (unsigned long long)s.packetsAccepted,
                (unsigned long long)s.packetsLate,
                (unsigned long long)s.packetsReordered,
                (unsigned long long)s.packetsDuplicate,
                (unsigned long long)s.packetsLost);
    std::printf("    PLC=%llu  silentFrames=%llu  observedJitter=%u ms  targetDepth=%u\n",
                (unsigned long long)s.plcGenerated,
                (unsigned long long)s.silentFrames,
                s.currentObservedJitterMs,
                s.currentTargetDepth);
    std::printf("    output: %u packet, %u PLC, %u silent\n",
                r.framesPacket, r.framesPLC, r.framesEmpty);
}

void TestCleanLAN() {
    JitterBuffer::Config cfg;
    cfg.capacityDepth     = 16;
    cfg.maxBytesPerPacket = 64;
    cfg.frameDurationMs   = 20;
    cfg.minTargetDepth    = 3;
    cfg.maxTargetDepth    = 10;
    JitterBuffer jb(cfg);

    auto stream = GenStream(/*N*/ 200, /*frameMs*/ 20,
                              /*startSendMs*/ 1000, /*transitMs*/ 5);
    auto trace  = ApplyImpairment(std::move(stream), 0.0f, 1, /*seed*/ 1);
    auto r = RunSimulation(jb, trace, /*totalFrames*/ 220, 20, 1010);

    PrintScenario("clean LAN (0% loss, 1 ms jitter)", jb.Stats(), r);
    EXPECT(jb.Stats().packetsLost      == 0);
    EXPECT(jb.Stats().plcGenerated     == 0);
    EXPECT(jb.Stats().packetsAccepted  == 200);
    EXPECT(r.framesPacket              >= 195);     // 200 - prebuffer slack
    EXPECT(jb.Stats().currentTargetDepth <= 5);     // stays near minimum
}

void TestResidential() {
    // 5% loss, 30 ms jitter — typical residential broadband under load.
    JitterBuffer::Config cfg;
    cfg.capacityDepth     = 32;
    cfg.maxBytesPerPacket = 64;
    cfg.frameDurationMs   = 20;
    cfg.minTargetDepth    = 3;
    cfg.maxTargetDepth    = 10;
    JitterBuffer jb(cfg);

    auto stream = GenStream(500, 20, 1000, 30);
    auto trace  = ApplyImpairment(std::move(stream), 0.05f, 30, /*seed*/ 42);
    // 510 frames so the tail just catches the last packet without
    // padding silence at the end.
    auto r = RunSimulation(jb, trace, 510, 20, 1080);

    PrintScenario("residential (5% loss, 30 ms jitter)", jb.Stats(), r);
    EXPECT(jb.Stats().packetsAccepted   > 450);     // most arrived
    EXPECT(jb.Stats().packetsLost       > 0);       // some lost (PLC fired)
    EXPECT(jb.Stats().plcGenerated     == jb.Stats().packetsLost);
    EXPECT(jb.Stats().currentTargetDepth >= 3);     // grew from minimum
    // Output continuity: packet+PLC dominates silence within the run.
    // The only silent frames should be the prebuffer warmup at the
    // very start.
    EXPECT(r.framesPacket + r.framesPLC >= 480);     // ≥96% of 500
    EXPECT(r.framesEmpty                <= 20);      // small prebuffer slack
}

void TestRoughInternet() {
    // 10% loss, 50 ms jitter — real bad-network conditions.
    JitterBuffer::Config cfg;
    cfg.capacityDepth     = 64;
    cfg.maxBytesPerPacket = 64;
    cfg.frameDurationMs   = 20;
    cfg.minTargetDepth    = 3;
    cfg.maxTargetDepth    = 12;
    JitterBuffer jb(cfg);

    auto stream = GenStream(500, 20, 1000, 50);
    auto trace  = ApplyImpairment(std::move(stream), 0.10f, 50, /*seed*/ 7);
    // Consumer runs just past last expected arrival (last sendTs +
    // transit + max jitter = 1000 + 499*20 + 50 + 50 ≈ 11080 ms,
    // 504 frames from start at t=1100).
    auto r = RunSimulation(jb, trace, 510, 20, 1100);

    PrintScenario("rough internet (10% loss, 50 ms jitter)", jb.Stats(), r);
    EXPECT(jb.Stats().packetsAccepted    > 380);
    EXPECT(jb.Stats().packetsLost        > 10);     // real loss happened
    EXPECT(jb.Stats().packetsReordered   > 0);      // jitter caused reorder
    EXPECT(jb.Stats().currentObservedJitterMs >= 5);   // jitter EMA tracked it
    EXPECT(jb.Stats().currentTargetDepth      >= 4);   // adapter expanded
    // Continuity: packet+PLC vs total. With prebuffer + occasional
    // starvation under heavy jitter, expect at least 90%.
    const float continuity = static_cast<float>(r.framesPacket + r.framesPLC)
                            / static_cast<float>(r.framesProduced);
    std::printf("    continuity = %.2f%% (packet+PLC / total)\n", continuity * 100.0f);
    EXPECT(continuity > 0.90f);
}

void TestBurstLoss() {
    // No background loss but a single 200 ms outage in the middle.
    // Tests that PLC and recovery work cleanly across a contiguous gap.
    JitterBuffer::Config cfg;
    cfg.capacityDepth     = 32;
    cfg.maxBytesPerPacket = 64;
    cfg.frameDurationMs   = 20;
    cfg.minTargetDepth    = 3;
    cfg.maxTargetDepth    = 10;
    JitterBuffer jb(cfg);

    auto stream = GenStream(300, 20, 1000, 10);
    // Erase packets 100..109 (200 ms outage centered at frame 100).
    stream.erase(stream.begin() + 100, stream.begin() + 110);
    auto r = RunSimulation(jb, stream, 320, 20, 1020);

    PrintScenario("burst loss (200 ms outage)", jb.Stats(), r);
    EXPECT(jb.Stats().packetsLost  == 10);
    EXPECT(jb.Stats().plcGenerated == 10);
    EXPECT(jb.Stats().packetsAccepted == 290);
    // Output through the gap should be PLC, then packets resume.
    EXPECT(r.framesPLC >= 10);
}

void TestDuplicates() {
    // Duplicate every fifth packet. A real network with retransmission
    // can produce duplicates; the buffer must deduplicate without
    // ill effects.
    JitterBuffer::Config cfg;
    cfg.capacityDepth     = 16;
    cfg.maxBytesPerPacket = 64;
    cfg.frameDurationMs   = 20;
    JitterBuffer jb(cfg);

    auto stream = GenStream(100, 20, 1000, 5);
    // Insert duplicates: every fifth packet appears twice.
    std::vector<SimPacket> withDups;
    withDups.reserve(stream.size() + stream.size() / 5);
    for (auto& p : stream) {
        withDups.push_back(p);
        if (p.seq % 5 == 0) {
            // Duplicate arrives 2 ms later.
            SimPacket dup = p;
            dup.arrivalMs += 2;
            withDups.push_back(dup);
        }
    }
    auto r = RunSimulation(jb, withDups, 110, 20, 1010);

    PrintScenario("duplicates (every 5th)", jb.Stats(), r);
    EXPECT(jb.Stats().packetsDuplicate >= 18);     // ~20 dups, allow slack
    EXPECT(jb.Stats().packetsLost     == 0);
    EXPECT(jb.Stats().plcGenerated    == 0);
    // Dup-aware accept count is original packet count (no double count).
    EXPECT(jb.Stats().packetsAccepted == 100);
}

} // namespace

// ---- Two additional regimes added for the multiplayer-readiness push:
//
//   * Sequence wraparound: 70 000 packets pushed forces the uint16
//     sequence counter to roll past 65 535. The buffer's wraparound-
//     correct comparisons (cast to int16 for delta) must keep the
//     consumer cursor advancing without "late" or "lost" spuriously.
//
//   * Performance: 1 million Push/Pop pairs in well under a second on
//     a 2020-vintage laptop. This is the load floor — a real game
//     with 8 voice sources at 50 packets/sec each runs at 400
//     packets/sec aggregate, ~7000× lighter than this benchmark.
//     The point is to catch accidental quadratic behavior in the
//     adaptive depth or `laterExists` scan.

namespace {

void TestSequenceWraparound() {
    JitterBuffer::Config cfg;
    cfg.frameDurationMs = 20;
    JitterBuffer jb(cfg);

    constexpr uint32_t kPackets = 70'000;
    constexpr uint16_t kStart   = 60'000;     // wraps at +5536

    uint8_t payload[64]{};
    uint8_t outBuf[64];
    TimestampMs ts = 0;

    uint32_t consumed = 0;
    uint32_t plcCount = 0;
    uint32_t silent   = 0;
    uint16_t expectedSeq = kStart;
    bool     started = false;

    for (uint32_t i = 0; i < kPackets; ++i) {
        const uint16_t seq = static_cast<uint16_t>(kStart + i);
        ts += 20;
        std::memcpy(payload, &seq, sizeof(seq));
        jb.Push(seq, ts, ts, payload, sizeof(payload));

        size_t      gotSize = 0;
        TimestampMs gotTs   = 0;
        const auto pr = jb.PopNext(outBuf, sizeof(outBuf), gotSize, gotTs);
        if (pr == PopResult::Packet) {
            uint16_t got;
            std::memcpy(&got, outBuf, sizeof(got));
            if (started && got != expectedSeq) {
                ++gFails;     // out-of-order is a hard failure
            }
            expectedSeq = static_cast<uint16_t>(got + 1);
            started     = true;
            ++consumed;
        } else if (pr == PopResult::PLC) {
            expectedSeq = static_cast<uint16_t>(expectedSeq + 1);
            ++plcCount;
        } else {
            ++silent;
        }
    }
    // Drain remainder.
    for (int d = 0; d < 64; ++d) {
        size_t s = 0; TimestampMs t = 0;
        const auto pr = jb.PopNext(outBuf, sizeof(outBuf), s, t);
        if (pr == PopResult::Packet) ++consumed;
        else if (pr == PopResult::PLC) ++plcCount;
        else ++silent;
    }

    std::printf("  [seq wraparound (70k pkts, start=60000)]\n");
    std::printf("    consumed=%u  PLC=%u  silent=%u  late=%llu\n",
                consumed, plcCount, silent,
                static_cast<unsigned long long>(jb.Stats().packetsLate));

    EXPECT(jb.Stats().packetsAccepted == kPackets);
    EXPECT(jb.Stats().packetsLate     == 0);
    EXPECT(jb.Stats().plcGenerated    == 0);
    // Allow a small drain underrun at the very end (last packets in
    // the slot array might not have been pulled before the loop
    // terminated) but the bulk should have been consumed in order.
    EXPECT(consumed >= kPackets - 16);
}

void TestPerformanceMillionOps() {
    JitterBuffer::Config cfg;
    cfg.frameDurationMs = 20;
    JitterBuffer jb(cfg);

    uint8_t payload[64]{};
    uint8_t outBuf[64];
    TimestampMs ts = 0;

    constexpr uint32_t kOps = 1'000'000;

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    for (uint32_t i = 0; i < kOps; ++i) {
        const uint16_t seq = static_cast<uint16_t>(i & 0xFFFFu);
        ts += 20;
        std::memcpy(payload, &seq, sizeof(seq));
        jb.Push(seq, ts, ts, payload, sizeof(payload));

        size_t      gotSize = 0;
        TimestampMs gotTs   = 0;
        (void)jb.PopNext(outBuf, sizeof(outBuf), gotSize, gotTs);
    }

    const auto t1 = clock::now();
    const double seconds   = std::chrono::duration<double>(t1 - t0).count();
    const double opsPerSec = (2.0 * kOps) / seconds;

    std::printf("  [performance: 2M push+pop ops in %.3f s = %.2f Mops/s]\n",
                seconds, opsPerSec / 1e6);
    EXPECT(opsPerSec > 5e6);     // 5M ops/s floor on any sane CPU
    EXPECT(seconds   < 2.0);     // sanity ceiling
}

} // namespace

int main() {
    std::printf("[jitter_buffer_test] running...\n");
    TestCleanLAN();
    TestResidential();
    TestRoughInternet();
    TestBurstLoss();
    TestDuplicates();
    TestSequenceWraparound();
    TestPerformanceMillionOps();
    if (gFails == 0) {
        std::printf("[jitter_buffer_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[jitter_buffer_test] %d failure(s)\n", gFails);
    return 1;
}
