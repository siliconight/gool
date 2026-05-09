// SPDX-License-Identifier: Apache-2.0
#include "audio_engine/runtime/replication_rate_limiter.h"

#include <algorithm>
#include <cstring>

namespace audio {

namespace {
constexpr size_t kCategoryCount =
    static_cast<size_t>(AudioCategory::Count);
}

void ReplicationRateLimiter::Initialize(
        const ReplicationRateLimitConfig& cfg) noexcept {
    cfg_ = cfg;

    // Clamp to at least 1 slot so FindOrAllocate has somewhere to land.
    const uint32_t n = std::max<uint32_t>(1, cfg.maxTrackedPlayers);
    slots_     = std::make_unique<PlayerSlot[]>(n);
    slotCount_ = n;
    // unique_ptr<PlayerSlot[]> default-constructs every PlayerSlot,
    // which zero-initializes the atomics and TokenBuckets.

    for (auto& c : totalRateLimited_) c.store(0, std::memory_order_relaxed);
    totalValidatorRejections_.store(0, std::memory_order_relaxed);
    totalPolicyViolations_.store(0, std::memory_order_relaxed);
    totalNewIdBudgetRejections_.store(0, std::memory_order_relaxed);
    lastSeenTick_       = 0;
    newPlayersThisTick_ = 0;

    // Pre-fill every bucket with its burst capacity so a brand-new
    // player can submit their burst-cap worth of events immediately
    // without needing to wait for a refill.
    for (size_t s = 0; s < slotCount_; ++s) {
        for (size_t c = 0; c < kCategoryCount; ++c) {
            const auto& b = cfg_.perCategory[c];
            slots_[s].buckets[c].tokens =
                (b.burstCapacity > 0)
                    ? static_cast<float>(b.burstCapacity)
                    : b.tokensPerSecond;
            slots_[s].buckets[c].lastRefillMs = 0;
        }
    }
}

void ReplicationRateLimiter::OnTickAdvanced(SimulationTick tick) noexcept {
    if (tick != lastSeenTick_) {
        lastSeenTick_       = tick;
        newPlayersThisTick_ = 0;
    }
}

ReplicationRateLimiter::PlayerSlot*
ReplicationRateLimiter::FindOrAllocate(AudioPlayerId id,
                                        TimestampMs   nowMs) noexcept {
    if (slotCount_ == 0) return nullptr;

    PlayerSlot* freeSlot = nullptr;
    PlayerSlot* lruSlot  = &slots_[0];

    for (size_t i = 0; i < slotCount_; ++i) {
        PlayerSlot& slot = slots_[i];
        if (slot.id == id) {
            slot.lastSeenMs = nowMs;
            return &slot;
        }
        if (slot.id == kInvalidPlayerId && freeSlot == nullptr) {
            freeSlot = &slot;
        }
        if (slot.lastSeenMs < lruSlot->lastSeenMs) {
            lruSlot = &slot;
        }
    }

    // This is a NEW player (not already in the table). Check the
    // per-tick admission budget before allocating a slot. If the
    // budget is 0 the cap is disabled.
    if (cfg_.maxNewPlayersPerTick > 0 &&
        newPlayersThisTick_ >= cfg_.maxNewPlayersPerTick) {
        totalNewIdBudgetRejections_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    ++newPlayersThisTick_;

    PlayerSlot* target = freeSlot ? freeSlot : lruSlot;

    // Recycle the slot for the new player. Reset buckets to full
    // capacity so a new player isn't penalized for the previous
    // tenant's spending.
    target->id         = id;
    target->lastSeenMs = nowMs;
    target->accepted.store(0, std::memory_order_relaxed);
    target->rateLimited.store(0, std::memory_order_relaxed);
    target->rejected.store(0, std::memory_order_relaxed);
    for (size_t c = 0; c < kCategoryCount; ++c) {
        const auto& b = cfg_.perCategory[c];
        target->buckets[c].tokens =
            (b.burstCapacity > 0)
                ? static_cast<float>(b.burstCapacity)
                : b.tokensPerSecond;
        target->buckets[c].lastRefillMs = nowMs;
    }
    return target;
}

ReplicationRateLimiter::PlayerSlot*
ReplicationRateLimiter::FindExisting(AudioPlayerId id) const noexcept {
    for (size_t i = 0; i < slotCount_; ++i) {
        if (slots_[i].id == id) {
            return const_cast<PlayerSlot*>(&slots_[i]);
        }
    }
    return nullptr;
}

bool ReplicationRateLimiter::TryAccept(AudioPlayerId playerId,
                                        AudioCategory category,
                                        TimestampMs   nowMs) noexcept {
    const auto cIdx = static_cast<size_t>(category);
    if (cIdx >= kCategoryCount) {
        // Out-of-range category is a host bug; accept by default
        // (fail-open) rather than silently dropping. The host's tests
        // will catch the bad value via behavior.
        return true;
    }

    const auto& budget = cfg_.perCategory[cIdx];

    PlayerSlot* slot = FindOrAllocate(playerId, nowMs);
    if (!slot) {
        // FindOrAllocate failed for one of two reasons:
        //   (a) slotCount_ == 0 (degenerate config) → fail-open, accept
        //   (b) per-tick new-player budget exhausted → fail-closed, reject
        // FindOrAllocate has already bumped totalNewIdBudgetRejections_
        // in case (b); the totals stat surfaces the reason.
        if (slotCount_ == 0) return true;
        return false;
    }

    // Unlimited category (tokensPerSecond <= 0): bypass the bucket
    // entirely. Note: the new-player budget above still gated this
    // call, so an id-cycling attacker can't bypass DoS protection
    // by spamming an "unlimited" category.
    if (budget.tokensPerSecond <= 0.0f) {
        slot->accepted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    TokenBucket& b   = slot->buckets[cIdx];
    const float cap  = (budget.burstCapacity > 0)
                          ? static_cast<float>(budget.burstCapacity)
                          : budget.tokensPerSecond;

    // Refill: integrate elapsed time at tokensPerSecond, clamped to cap.
    if (nowMs > b.lastRefillMs) {
        const float elapsedSec =
            static_cast<float>(nowMs - b.lastRefillMs) / 1000.0f;
        b.tokens = std::min(cap, b.tokens + elapsedSec * budget.tokensPerSecond);
        b.lastRefillMs = nowMs;
    } else if (b.lastRefillMs == 0 && nowMs == 0) {
        // Newly allocated slot, host hasn't called OnTickAdvanced yet.
        // The slot was pre-filled to capacity by FindOrAllocate so the
        // first burst-cap events are accepted; nothing more to do.
        b.lastRefillMs = nowMs;
    }

    if (b.tokens >= 1.0f) {
        b.tokens -= 1.0f;
        slot->accepted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Bucket empty: reject.
    slot->rateLimited.fetch_add(1, std::memory_order_relaxed);
    totalRateLimited_[cIdx].fetch_add(1, std::memory_order_relaxed);
    return false;
}

void ReplicationRateLimiter::RecordValidatorRejection(
        AudioPlayerId playerId) noexcept {
    totalValidatorRejections_.fetch_add(1, std::memory_order_relaxed);
    // Use FindExisting (not FindOrAllocate) so a validator-rejected
    // event from an unknown playerId doesn't consume a slot in our
    // LRU table — otherwise the validator hook becomes its own DoS
    // surface (an attacker could blow out the slot table just by
    // sending events the validator will reject anyway). Per-player
    // counters update only for already-tracked players; the global
    // total is bumped unconditionally above.
    if (auto* slot = FindExisting(playerId); slot) {
        slot->rejected.fetch_add(1, std::memory_order_relaxed);
    }
}

void ReplicationRateLimiter::RecordPolicyViolation(
        AudioPlayerId playerId) noexcept {
    totalPolicyViolations_.fetch_add(1, std::memory_order_relaxed);
    // Same FindExisting semantics as validator rejection: a spoof
    // attempt from an unknown playerId must not reserve a slot.
    // Per-player counters update only for already-tracked players;
    // the global total above is the canonical signal.
    if (auto* slot = FindExisting(playerId); slot) {
        slot->rejected.fetch_add(1, std::memory_order_relaxed);
    }
}

uint64_t ReplicationRateLimiter::TotalRateLimitedForCategory(
        AudioCategory category) const noexcept {
    const auto cIdx = static_cast<size_t>(category);
    if (cIdx >= kCategoryCount) return 0;
    return totalRateLimited_[cIdx].load(std::memory_order_relaxed);
}

uint64_t ReplicationRateLimiter::TotalValidatorRejections() const noexcept {
    return totalValidatorRejections_.load(std::memory_order_relaxed);
}

uint64_t ReplicationRateLimiter::TotalPolicyViolations() const noexcept {
    return totalPolicyViolations_.load(std::memory_order_relaxed);
}

uint64_t ReplicationRateLimiter::TotalNewIdBudgetRejections() const noexcept {
    return totalNewIdBudgetRejections_.load(std::memory_order_relaxed);
}

bool ReplicationRateLimiter::GetPlayerStats(
        AudioPlayerId               playerId,
        PerPlayerReplicationStats*  out) const noexcept {
    if (!out) return false;
    PlayerSlot* slot = FindExisting(playerId);
    if (!slot) return false;
    out->eventsAccepted    = slot->accepted.load(std::memory_order_relaxed);
    out->eventsRateLimited = slot->rateLimited.load(std::memory_order_relaxed);
    out->eventsRejected    = slot->rejected.load(std::memory_order_relaxed);
    return true;
}

} // namespace audio
