// SPDX-License-Identifier: Apache-2.0
//
// ReplicationRateLimiter: per-player, per-category token-bucket limiter
// driven against the runtime's deterministic server clock. Sized once at
// Initialize() from AudioConfig::replicationRateLimit; allocates nothing
// thereafter.
//
// Threading: all public methods are called from the network thread (the
// thread that calls SubmitReplicatedEvent / OnTickAdvanced). Counters
// exposed for cross-thread reads via std::atomic<uint64_t> with relaxed
// ordering so the game thread's GetStats() can read them safely.
//
// Determinism: the clock is the host's serverTimeMs (passed in via
// OnTickAdvanced and read here through latestServerTimeMs_). For a fixed
// input timeline, rate-limit decisions are bit-identical across runs.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#include "audio_engine/config.h"
#include "audio_engine/types.h"

namespace audio {

struct PerPlayerReplicationStats {
    uint64_t eventsAccepted   = 0;
    uint64_t eventsRateLimited = 0;
    uint64_t eventsRejected   = 0; // by IReplicationValidator
};

class ReplicationRateLimiter {
public:
    ReplicationRateLimiter()  = default;
    ~ReplicationRateLimiter() = default;

    ReplicationRateLimiter(const ReplicationRateLimiter&) = delete;
    ReplicationRateLimiter& operator=(const ReplicationRateLimiter&) = delete;

    // Sized once. Allocates the per-player slot table and zeros all
    // counters. Subsequent calls re-initialize.
    void Initialize(const ReplicationRateLimitConfig& cfg) noexcept;

    // Called by AudioRuntime::OnTickAdvanced. Resets the per-tick
    // new-player admission counter so the next tick's budget is fresh.
    // Cheap; safe to call every server tick.
    void OnTickAdvanced(SimulationTick tick) noexcept;

    // Network thread. Returns true if the event is accepted (token
    // consumed); false if rate-limited (no token available, or the
    // per-tick new-player cap was hit). Updates per-player and
    // per-category counters either way.
    //
    // `nowMs` is the deterministic clock — the runtime passes the most
    // recent serverTimeMs from OnTickAdvanced. For a host that hasn't
    // called OnTickAdvanced yet, nowMs == 0 is safe; the bucket starts
    // full so the first event is always accepted.
    bool TryAccept(AudioPlayerId playerId,
                   AudioCategory category,
                   TimestampMs   nowMs) noexcept;

    // Network thread. Records that the validator rejected an event.
    // Used by AudioRuntime when IReplicationValidator::ShouldAccept
    // returns false; counted separately from rate-limit drops.
    void RecordValidatorRejection(AudioPlayerId playerId) noexcept;

    // Network thread. Records that the runtime's built-in
    // replication-policy enforcement (Phase 2.5) rejected an event —
    // a Client-sourced event declared a ServerAuthoritative policy,
    // which only the server is allowed to author. Tracked separately
    // from validator rejections so observability dashboards can
    // distinguish "runtime caught a spoof attempt" from "host's
    // custom validator said no."
    void RecordPolicyViolation(AudioPlayerId playerId) noexcept;

    // Game thread (read-only). Aggregate counter across all players for
    // the given category. Safe to poll every frame.
    uint64_t TotalRateLimitedForCategory(AudioCategory category) const noexcept;

    // Game thread (read-only). Aggregate counter for events rejected by
    // the IReplicationValidator hook (across all players).
    uint64_t TotalValidatorRejections() const noexcept;

    // Game thread (read-only). Aggregate counter for events rejected
    // by Phase 2.5 replication-policy enforcement (Client-sourced
    // events claiming ServerAuthoritative). Separate from validator
    // rejections so dashboards can distinguish runtime spoof catches
    // from host-policy denials.
    uint64_t TotalPolicyViolations() const noexcept;

    // Game thread (read-only). Aggregate counter for events rejected
    // because the per-tick new-player admission cap was exhausted.
    // Non-zero values indicate either a legitimately busy lobby
    // (bump `maxNewPlayersPerTick`) or a playerId-cycling DoS.
    uint64_t TotalNewIdBudgetRejections() const noexcept;

    // Game thread (read-only). Returns false if the player is unknown
    // (never submitted an event in this session, or was evicted from
    // the LRU table). Otherwise fills `out` with the player's
    // accept/limit/reject counters.
    bool GetPlayerStats(AudioPlayerId               playerId,
                        PerPlayerReplicationStats*  out) const noexcept;

private:
    struct TokenBucket {
        float       tokens       = 0.0f;
        TimestampMs lastRefillMs = 0;
    };

    struct PlayerSlot {
        AudioPlayerId id            = kInvalidPlayerId;
        TimestampMs   lastSeenMs    = 0;
        // Per-category buckets. Indexed by AudioCategory.
        TokenBucket   buckets[6]{};
        // Counters use std::atomic so the game thread can read them
        // while the network thread updates them. Relaxed ordering is
        // sufficient — these are monotonic counters with no
        // happens-before relationship to anything else.
        std::atomic<uint64_t> accepted{0};
        std::atomic<uint64_t> rateLimited{0};
        std::atomic<uint64_t> rejected{0};
    };

    PlayerSlot* FindOrAllocate(AudioPlayerId id, TimestampMs nowMs) noexcept;
    PlayerSlot* FindExisting(AudioPlayerId id) const noexcept;

    ReplicationRateLimitConfig                cfg_{};
    std::unique_ptr<PlayerSlot[]>             slots_;
    size_t                                    slotCount_ = 0;
    std::array<std::atomic<uint64_t>, 6>      totalRateLimited_{};
    std::atomic<uint64_t>                     totalValidatorRejections_{0};
    std::atomic<uint64_t>                     totalPolicyViolations_{0};
    std::atomic<uint64_t>                     totalNewIdBudgetRejections_{0};

    // Per-tick new-player admission tracking. lastSeenTick_ detects
    // tick advance; newPlayersThisTick_ counts admissions in the
    // current tick. Both touched only on the network thread.
    SimulationTick                            lastSeenTick_       = 0;
    uint32_t                                  newPlayersThisTick_ = 0;
};

} // namespace audio
