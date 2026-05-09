// audio_engine/voice/jitter_buffer.h
//
// Adaptive jitter buffer for real-network voice. Sits between the
// network ingest (Push) and the control-thread decode pump (Pop). It
// solves four problems that a fixed-depth reorder buffer doesn't:
//
//   * Adaptation. Buffer depth grows under network churn (rising
//     observed jitter) and shrinks when the network stabilizes. A
//     fixed depth is a tradeoff between latency on a clean network
//     and gappiness on a bad one; an adaptive buffer matches the
//     conditions it actually sees.
//
//   * Prebuffering. When a stream first comes up, the consumer waits
//     until the target depth is filled before starting to decode.
//     This is the cost of resilience: the first few frames of every
//     stream incur ~targetDepth × frameDurationMs of latency in
//     exchange for not immediately running dry on the first dropped
//     packet.
//
//   * PLC integration. When a packet at the expected sequence is
//     missing but later packets exist (a real loss, not just a
//     reorder), Pop signals PLC instead of returning either silence
//     or nothing. The control thread then asks the codec to
//     extrapolate (Opus has built-in PLC; the stub writes silence).
//
//   * Telemetry. Per-buffer counters for received, late, lost,
//     reordered, duplicate, and PLC-generated packets, plus the
//     current observed jitter and target depth, surfaced to the
//     host so the game UI can show "your voice is dropping".
//
// All hot paths are allocation-free. Memory is sized once at
// construction (capacityDepth slots, each maxBytesPerPacket bytes)
// and never grows. Single-producer (network thread) /
// single-consumer (control thread) discipline is preserved via one
// atomic per slot; no other synchronization is needed.
//
// RFC 3550 reference: the inter-arrival jitter formula is the standard
// one used by RTP, applied here on the millisecond timestamps the host
// stamps each packet with. With 20 ms voice frames the per-frame
// granularity is 20 ms; jitter on an order-of-magnitude smaller scale
// than that doesn't drive adaptation, which is the right behavior.

#ifndef AUDIO_ENGINE_VOICE_JITTER_BUFFER_H
#define AUDIO_ENGINE_VOICE_JITTER_BUFFER_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "audio_engine/types.h"

namespace audio::voice {

enum class PopResult : uint8_t {
    // Packet payload copied to the output buffer; consumer should
    // hand this to codec.Decode().
    Packet  = 0,
    // Sequence at the expected position is lost (later packets exist
    // but this one didn't arrive); consumer should call codec.DecodeLost()
    // to produce concealment audio. expectedSeq has advanced.
    PLC     = 1,
    // Buffer is empty or still prebuffering; consumer writes silence
    // for this frame. expectedSeq does NOT advance — we're waiting.
    Empty   = 2,
};

// Per-buffer counters. Each counter has exactly one writer (producer
// for push counters, consumer for pop counters), so reads from a third
// thread are race-free in practice — uint64 monotone increments are
// not torn on any platform we support.
struct JitterBufferStats {
    uint64_t packetsReceived        = 0;     // any Push that wasn't dropped at the door
    uint64_t packetsAccepted        = 0;     // landed in a slot
    uint64_t packetsLate            = 0;     // arrived past consumption point; dropped
    uint64_t packetsDuplicate       = 0;     // same seq twice
    uint64_t packetsReordered       = 0;     // arrived before its predecessor (still accepted)
    uint64_t packetsLost            = 0;     // expected seq never arrived; PLC issued
    uint64_t packetsOverwritten     = 0;     // capacity full; oldest evicted (network ahead of consumer)
    uint64_t plcGenerated           = 0;     // Pop returned PLC
    uint64_t silentFrames           = 0;     // Pop returned Empty (prebuffering or starved)
    uint32_t currentTargetDepth     = 0;     // frames the consumer is currently aiming to keep buffered
    uint32_t currentObservedJitterMs = 0;    // EMA of inter-arrival jitter, in ms
};

class JitterBuffer {
public:
    struct Config {
        // Maximum depth (slot count). Hard cap on storage. Worst-case
        // memory: capacityDepth × maxBytesPerPacket bytes per buffer.
        uint32_t capacityDepth      = 32;
        // Maximum packet payload in bytes.
        uint32_t maxBytesPerPacket  = 1500;
        // Frame duration in ms (Opus default 20 ms). Used to convert
        // jitter (ms) to slot counts.
        uint32_t frameDurationMs    = 20;
        // Initial / minimum target depth in frames. Even on a perfectly
        // stable network the buffer keeps this much headroom.
        uint32_t minTargetDepth     = 3;     // ~60 ms at 20 ms frames
        // Hard ceiling for adaptive target. Prevents unbounded latency
        // growth when jitter becomes pathological.
        uint32_t maxTargetDepth     = 10;    // ~200 ms at 20 ms frames
        // EMA smoothing factor for the jitter estimate, expressed as
        // 1 / 2^shift. RFC 3550 default is 1/16 (shift=4); weights
        // recent jitter heavily without overreacting.
        uint32_t jitterEmaShift     = 4;
    };

    explicit JitterBuffer(const Config& cfg)
        : cfg_(cfg),
          slots_(cfg.capacityDepth) {
        for (auto& s : slots_) {
            s.bytes.resize(cfg.maxBytesPerPacket);
            s.size = 0;
            s.seq  = 0;
            s.valid.store(false, std::memory_order_relaxed);
        }
        targetDepth_ = cfg.minTargetDepth;
    }

    // Convenience for the legacy (depth, maxBytes) callers — same
    // signature the previous fixed-depth buffer exposed.
    JitterBuffer(uint32_t depth, uint32_t maxBytesPerPacket)
        : JitterBuffer(MakeConfigFromLegacy(depth, maxBytesPerPacket)) {}

    JitterBuffer(const JitterBuffer&)            = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;

    // ---- Network thread ----------------------------------------------

    // Push a packet. `arrivalMs` is the host's wall-clock arrival time
    // (use the same clock the host stamps `sendTs` from); both feed
    // the inter-arrival jitter estimate.
    bool Push(uint16_t       seq,
                TimestampMs    sendTs,
                TimestampMs    arrivalMs,
                const uint8_t* data,
                size_t         size) noexcept {
        ++stats_.packetsReceived;

        if (size > cfg_.maxBytesPerPacket) {
            ++stats_.packetsLate;
            return false;
        }

        // Update inter-arrival jitter (RFC 3550). On the very first
        // packet (or after Reset) seed the timing baseline only.
        if (jitterSeeded_) {
            const int64_t transit = static_cast<int64_t>(arrivalMs)
                                  - static_cast<int64_t>(sendTs);
            const int64_t d    = transit - prevTransit_;
            const int64_t absD = (d < 0) ? -d : d;
            const int64_t adj  = (absD - static_cast<int64_t>(jitterMs_))
                               >> cfg_.jitterEmaShift;
            const int64_t newJ = static_cast<int64_t>(jitterMs_) + adj;
            jitterMs_     = (newJ < 0) ? 0u : static_cast<uint32_t>(newJ);
            prevTransit_  = transit;
        } else {
            prevTransit_  = static_cast<int64_t>(arrivalMs)
                          - static_cast<int64_t>(sendTs);
            jitterSeeded_ = true;
        }
        stats_.currentObservedJitterMs = jitterMs_;

        // Sequence: drop packets older than the consumer cursor. Cast
        // to int16_t for wraparound-correct comparison.
        if (consumerStarted_) {
            const int16_t deltaFromExpected = static_cast<int16_t>(seq - expectedSeq_);
            if (deltaFromExpected < 0) {
                ++stats_.packetsLate;
                return false;
            }
        }

        const uint32_t idx = seq % cfg_.capacityDepth;
        Slot& s = slots_[idx];

        if (s.valid.load(std::memory_order_acquire)) {
            if (s.seq == seq) {
                ++stats_.packetsDuplicate;
                return false;
            }
            // Slot busy with a different sequence: producer is running
            // ahead of the consumer. Overwrite is the right policy
            // (we're network-bound, not memory-bound) but count it.
            ++stats_.packetsOverwritten;
        }

        // Reorder check vs the highest seq we've seen so far.
        if (highestSeenSeqValid_) {
            const int16_t deltaFromHighest =
                static_cast<int16_t>(seq - highestSeenSeq_);
            if (deltaFromHighest < 0) {
                ++stats_.packetsReordered;
            } else {
                highestSeenSeq_ = seq;
            }
        } else {
            highestSeenSeq_      = seq;
            highestSeenSeqValid_ = true;
        }

        if (data && size > 0) std::memcpy(s.bytes.data(), data, size);
        s.size = static_cast<uint32_t>(size);
        s.seq  = seq;
        s.ts   = sendTs;
        s.valid.store(true, std::memory_order_release);
        ++stats_.packetsAccepted;
        return true;
    }

    // ---- Control thread ----------------------------------------------

    PopResult PopNext(uint8_t*      out,
                        size_t        outCapacity,
                        size_t&       outSize,
                        TimestampMs&  outTs) noexcept {
        outSize = 0;
        outTs   = 0;

        // Phase 1: prebuffer. Wait until target depth is filled before
        // starting the consumer cursor.
        if (!consumerStarted_) {
            uint32_t valid = 0;
            uint16_t lowestSeq = 0;
            bool     haveLow   = false;
            for (uint32_t i = 0; i < cfg_.capacityDepth; ++i) {
                if (slots_[i].valid.load(std::memory_order_acquire)) {
                    if (!haveLow) {
                        lowestSeq = slots_[i].seq;
                        haveLow   = true;
                    } else {
                        const int16_t d = static_cast<int16_t>(slots_[i].seq - lowestSeq);
                        if (d < 0) lowestSeq = slots_[i].seq;
                    }
                    ++valid;
                }
            }
            if (valid < targetDepth_) {
                ++stats_.silentFrames;
                return PopResult::Empty;
            }
            expectedSeq_     = lowestSeq;
            consumerStarted_ = true;
        }

        // Phase 2: adapt target depth from observed jitter.
        AdaptTargetDepth();

        const uint32_t idx = expectedSeq_ % cfg_.capacityDepth;
        Slot& s = slots_[idx];

        if (s.valid.load(std::memory_order_acquire) && s.seq == expectedSeq_) {
            const size_t copy = (s.size < outCapacity) ? s.size : outCapacity;
            if (copy > 0) std::memcpy(out, s.bytes.data(), copy);
            outSize = copy;
            outTs   = s.ts;
            s.valid.store(false, std::memory_order_release);
            ++expectedSeq_;
            return PopResult::Packet;
        }

        // Miss. Decide loss vs drained.
        bool laterExists = false;
        for (uint32_t i = 0; i < cfg_.capacityDepth; ++i) {
            if (slots_[i].valid.load(std::memory_order_acquire)) {
                const int16_t d = static_cast<int16_t>(slots_[i].seq - expectedSeq_);
                if (d > 0) { laterExists = true; break; }
            }
        }

        if (laterExists) {
            ++stats_.packetsLost;
            ++stats_.plcGenerated;
            ++expectedSeq_;
            return PopResult::PLC;
        }

        ++stats_.silentFrames;
        return PopResult::Empty;
    }

    // ---- Lifecycle ---------------------------------------------------

    // Reset all consumer-side state and clear all slots. Use when a
    // stream is known to have restarted (player reconnected, voice
    // re-enabled). Counters are NOT reset by default — they're
    // cumulative for telemetry; pass true to also zero counters.
    void Reset(bool clearStats = false) noexcept {
        for (auto& s : slots_) {
            s.valid.store(false, std::memory_order_relaxed);
            s.size = 0;
        }
        consumerStarted_     = false;
        expectedSeq_         = 0;
        highestSeenSeqValid_ = false;
        highestSeenSeq_      = 0;
        jitterSeeded_        = false;
        jitterMs_            = 0;
        prevTransit_         = 0;
        if (clearStats) stats_ = JitterBufferStats{};
        targetDepth_ = cfg_.minTargetDepth;
    }

    const JitterBufferStats& Stats() const noexcept { return stats_; }
    uint32_t Depth()             const noexcept { return cfg_.capacityDepth; }
    uint32_t TargetDepth()       const noexcept { return targetDepth_; }
    uint32_t ObservedJitterMs()  const noexcept { return jitterMs_; }

private:
    static Config MakeConfigFromLegacy(uint32_t depth, uint32_t maxBytes) noexcept {
        Config c;
        c.capacityDepth     = depth;
        c.maxBytesPerPacket = maxBytes;
        return c;
    }

    void AdaptTargetDepth() noexcept {
        const uint32_t framesPerJitter = (jitterMs_ + cfg_.frameDurationMs - 1)
                                       / cfg_.frameDurationMs;
        uint32_t want = 2u * framesPerJitter;
        if (want < cfg_.minTargetDepth) want = cfg_.minTargetDepth;
        if (want > cfg_.maxTargetDepth) want = cfg_.maxTargetDepth;
        if (want > targetDepth_)        targetDepth_ += 1;
        else if (want < targetDepth_)   targetDepth_ -= 1;
        stats_.currentTargetDepth = targetDepth_;
    }

    struct Slot {
        std::atomic<bool>    valid{false};
        uint16_t             seq  = 0;
        uint32_t             size = 0;
        TimestampMs          ts   = 0;
        std::vector<uint8_t> bytes;
    };

    Config            cfg_;
    std::vector<Slot> slots_;

    // Producer state.
    bool        jitterSeeded_        = false;
    int64_t     prevTransit_         = 0;
    uint32_t    jitterMs_            = 0;
    uint16_t    highestSeenSeq_      = 0;
    bool        highestSeenSeqValid_ = false;

    // Consumer state.
    bool      consumerStarted_ = false;
    uint16_t  expectedSeq_     = 0;
    uint32_t  targetDepth_     = 0;

    JitterBufferStats stats_{};
};

} // namespace audio::voice

#endif // AUDIO_ENGINE_VOICE_JITTER_BUFFER_H
