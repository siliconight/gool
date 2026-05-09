// SPDX-License-Identifier: Apache-2.0
//
// DefaultBoundsValidator: a shipped IReplicationValidator that rejects
// AudioEvents with malformed numeric fields. Designed to compose with
// host-supplied validators via ChainReplicationValidator (below).
//
// What it catches:
//   * NaN / +Inf / -Inf in any vec3 field (position, forward, velocity)
//     — the existing spatializer / mixer math is not NaN-safe and a
//     single such event poisons the per-voice biquad state.
//   * Vec3 magnitudes beyond a configured threshold — game-world
//     scales are bounded; an event at (1e30, 0, 0) is either a bug
//     or an attack.
//   * NaN / Inf / extreme magnitudes in `parameterValue` and
//     `parameterSmoothingMs`.
//   * (Optional) unknown soundIds via a host-supplied lookup callback.
//
// What it does NOT catch:
//   * Replication-policy spoofing (use SubmitReplicatedEvent's
//     2-arg overload with `ReplicationSource::Client`)
//   * Authentication of `event.playerId` (the runtime cannot know
//     which network peer sent which packet)
//   * Logical anti-cheat (gunshot rate vs gameplay state, etc.)
//
// Threading: ShouldAccept() runs on the network thread. The validator
// holds atomic counters for stats exposed via GetStats(); reading those
// from the game thread is race-free.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include "audio_engine/events.h"
#include "audio_engine/replication_validator.h"
#include "audio_engine/types.h"

namespace audio {

struct DefaultBoundsValidatorConfig {
    // Reject vec3 fields whose Euclidean magnitude exceeds these.
    // Defaults are well beyond any plausible game-world scale; tighten
    // if your world is smaller and you want stricter rejection.
    float maxPositionMagnitude         = 1.0e6f;   // 1000 km
    float maxVelocityMagnitude         = 1.0e5f;   // 100 km/s

    // Reject parameter values with abs() exceeding these.
    float maxAbsParameterValue         = 1.0e6f;
    float maxAbsParameterSmoothingMs   = 60'000.0f; // 1 minute

    // Optional soundId lookup: if set, ShouldAccept calls this for
    // every event carrying a non-zero `soundId`. Returning false
    // rejects the event. Pass a closure that queries your asset
    // registry / sound bank. Leave default-constructed (empty) to
    // skip soundId validation.
    //
    // The callback runs on the network thread and must be reentrant.
    // It MUST be allocation-free at call time for production
    // deployments; the validator stores it verbatim.
    std::function<bool(AudioSoundId)> soundIdIsKnown;
};

class DefaultBoundsValidator final : public IReplicationValidator {
public:
    DefaultBoundsValidator() = default;
    explicit DefaultBoundsValidator(DefaultBoundsValidatorConfig cfg)
        : cfg_(std::move(cfg)) {}

    bool ShouldAccept(const AudioEvent& event,
                      AudioPlayerId     playerId) noexcept override;

    // Per-rejection-reason counters. Cumulative. Game-thread reads
    // are race-free thanks to atomics with relaxed ordering.
    struct Stats {
        uint64_t rejectedNonFiniteVec3    = 0;
        uint64_t rejectedExtremePosition  = 0;
        uint64_t rejectedExtremeVelocity  = 0;
        uint64_t rejectedNonFiniteParam   = 0;
        uint64_t rejectedExtremeParam     = 0;
        uint64_t rejectedUnknownSound     = 0;
    };
    Stats GetStats() const noexcept;

private:
    DefaultBoundsValidatorConfig          cfg_{};
    mutable std::atomic<uint64_t>         cNonFiniteVec3_{0};
    mutable std::atomic<uint64_t>         cExtremePos_{0};
    mutable std::atomic<uint64_t>         cExtremeVel_{0};
    mutable std::atomic<uint64_t>         cNonFiniteParam_{0};
    mutable std::atomic<uint64_t>         cExtremeParam_{0};
    mutable std::atomic<uint64_t>         cUnknownSound_{0};
};

// ChainReplicationValidator: composes multiple IReplicationValidator
// instances. ShouldAccept returns true only if every chained
// validator accepts. Short-circuits on the first rejection — later
// validators are not called.
//
// Use this to combine the shipped DefaultBoundsValidator with your
// own anti-cheat / game-mode-specific validator without writing the
// composition glue yourself.
class ChainReplicationValidator final : public IReplicationValidator {
public:
    ChainReplicationValidator() = default;

    // Append a validator to the chain. Lifetime is the host's; the
    // chain stores the pointer verbatim and never deletes it. Pass
    // a stable address — typically a member of your runtime struct
    // or a static global.
    void Add(IReplicationValidator* v) noexcept {
        if (count_ < kMaxChain && v != nullptr) chain_[count_++] = v;
    }

    void Clear() noexcept { count_ = 0; }

    bool ShouldAccept(const AudioEvent& event,
                      AudioPlayerId     playerId) noexcept override {
        for (size_t i = 0; i < count_; ++i) {
            if (!chain_[i]->ShouldAccept(event, playerId)) return false;
        }
        return true;
    }

private:
    static constexpr size_t        kMaxChain = 8;
    IReplicationValidator*         chain_[kMaxChain]{};
    size_t                         count_ = 0;
};

} // namespace audio
