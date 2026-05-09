// tests/unit/replication_rate_limit_test.cpp
//
// Validates the per-player, per-category replication rate limiter and
// the IReplicationValidator host hook installed by Phase 2.3.
//
// Coverage:
//   1. A burst within capacity is fully accepted; the (burst+1)-th
//      event in the same tick is rejected with AudioResult::RateLimited.
//   2. After serverTimeMs advances, tokens refill at tokensPerSecond
//      and rejected traffic resumes accepting.
//   3. Per-player isolation: one spammer hitting their cap doesn't
//      reduce another player's available tokens.
//   4. Per-category isolation: hitting SFX cap doesn't reduce Music
//      or Voice budgets.
//   5. UI's default of 0 tokensPerSecond means unlimited (the runtime
//      treats <=0 as "no limit"), so any number of events accepts.
//   6. IReplicationValidator returning false drops the event with
//      AudioResult::PolicyViolation, increments
//      Stats::replicationEventsRejectedByValidator, and the event
//      doesn't reach the control thread.
//   7. Per-player stats counters (eventsAccepted, eventsRateLimited,
//      eventsRejected) match the actual driving sequence.
//   8. Determinism: two runs with the same OnTickAdvanced sequence
//      and the same SubmitReplicatedEvent calls produce identical
//      accept/reject decisions.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/events.h"
#include "audio_engine/replication_validator.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

namespace {

class OfflineBackend final : public audio::IAudioBackend {
public:
    audio::AudioResult Start(const audio::AudioBackendConfig& cfg,
                              audio::IAudioRenderCallback*    cb) override {
        cfg_ = cfg; cb_ = cb;
        scratch_.assign(static_cast<size_t>(cfg.bufferSize) * cfg.channels, 0.0f);
        return audio::AudioResult::Success;
    }
    void     Stop() override { cb_ = nullptr; }
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "OfflineRateLimit"; }

    // Pull `frames` of mixed audio into `out`. Used for audibility
    // assertions (e.g. "rejected event produced no rendered audio").
    void Render(uint32_t frames, std::vector<float>& out) {
        out.clear();
        if (!cb_) return;
        const uint32_t bs = cfg_.bufferSize, ch = cfg_.channels;
        out.reserve(static_cast<size_t>(frames + bs) * ch);
        uint32_t produced = 0;
        while (produced < frames) {
            const uint32_t take = std::min<uint32_t>(bs, frames - produced);
            std::fill(scratch_.begin(), scratch_.end(), 0.0f);
            cb_->OnRender(scratch_.data(), take, ch);
            out.insert(out.end(), scratch_.begin(),
                       scratch_.begin() + static_cast<ptrdiff_t>(take * ch));
            produced += take;
        }
    }

private:
    audio::AudioBackendConfig    cfg_{};
    audio::IAudioRenderCallback* cb_ = nullptr;
    std::vector<float>           scratch_;
};

// Validator that rejects every event from a specific player.
class BlockOnePlayerValidator final : public audio::IReplicationValidator {
public:
    explicit BlockOnePlayerValidator(audio::AudioPlayerId blocked)
        : blocked_(blocked) {}

    bool ShouldAccept(const audio::AudioEvent&,
                      audio::AudioPlayerId pid) noexcept override {
        return pid != blocked_;
    }
private:
    audio::AudioPlayerId blocked_;
};

// Returns the OfflineBackend pointer (still owned by the runtime via
// the dependencies struct) so tests that need to render audio can pull
// from it.
OfflineBackend* InitRuntime(audio::AudioRuntime& rt,
                             uint32_t sfxBurst   = 50,
                             float    sfxPerSec  = 50.0f) {
    audio::AudioConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    cfg.outputMode = audio::AudioOutputMode::Stereo;
    cfg.replicationRateLimit.perCategory[
        static_cast<size_t>(audio::AudioCategory::SFX)] = {sfxPerSec, sfxBurst};

    auto backend = std::make_unique<OfflineBackend>();
    OfflineBackend* raw = backend.get();

    audio::AudioRuntimeDependencies deps;
    deps.backend = std::move(backend);

    auto rc = rt.Initialize(cfg, std::move(deps));
    assert(rc == audio::AudioResult::Success);
    return raw;
}

audio::AudioEvent MakeSfx(audio::AudioPlayerId pid) {
    audio::AudioEvent ev = audio::AudioEvent::MakePlaySoundAtLocation(
        /*sound*/ 0xABCDu, audio::Vec3{0, 0, 0});
    ev.playerId = pid;
    ev.category = audio::AudioCategory::SFX;
    return ev;
}

audio::AudioEvent MakeMusic(audio::AudioPlayerId pid) {
    audio::AudioEvent ev = audio::AudioEvent::MakePlaySoundAtLocation(
        /*sound*/ 0xDEADu, audio::Vec3{0, 0, 0});
    ev.playerId = pid;
    ev.category = audio::AudioCategory::Music;
    return ev;
}

void TestBurstThenReject() {
    std::cout << "  [burst within capacity accepts; over-cap rejects]\n";
    audio::AudioRuntime rt; InitRuntime(rt, /*sfxBurst*/ 5, /*sfxPerSec*/ 5.0f);
    rt.OnTickAdvanced(1, /*serverMs*/ 1000);

    int accepted = 0, rejected = 0;
    for (int i = 0; i < 20; ++i) {
        auto rc = rt.SubmitReplicatedEvent(MakeSfx(/*pid*/ 7));
        if (rc == audio::AudioResult::Success)     ++accepted;
        else if (rc == audio::AudioResult::RateLimited) ++rejected;
        else { std::cerr << "    unexpected rc=" << int(rc) << "\n"; assert(false); }
    }
    std::cout << "    accepted=" << accepted << " rejected=" << rejected << "\n";
    assert(accepted == 5);
    assert(rejected == 15);

    // Aggregate counter reflects the rejections.
    auto s = rt.GetStats();
    assert(s.replicationEventsRateLimited[
        static_cast<size_t>(audio::AudioCategory::SFX)] == 15u);
    assert(s.replicationEventsRejectedByValidator == 0u);

    // Per-player stats also.
    audio::AudioRuntime::PerPlayerReplicationStats pp{};
    bool found = rt.GetPerPlayerReplicationStats(7, pp);
    assert(found);
    assert(pp.eventsAccepted    == 5u);
    assert(pp.eventsRateLimited == 15u);
    assert(pp.eventsRejected    == 0u);

    rt.Shutdown();
}

void TestRefillOverTime() {
    std::cout << "  [tokens refill at the configured rate]\n";
    audio::AudioRuntime rt; InitRuntime(rt, /*burst*/ 3, /*perSec*/ 10.0f);
    rt.OnTickAdvanced(1, 0);

    // Drain the bucket.
    for (int i = 0; i < 3; ++i) {
        auto rc = rt.SubmitReplicatedEvent(MakeSfx(1));
        assert(rc == audio::AudioResult::Success);
    }
    auto rcEmpty = rt.SubmitReplicatedEvent(MakeSfx(1));
    assert(rcEmpty == audio::AudioResult::RateLimited);

    // Advance 500 ms. At 10 tokens/sec that's 5 tokens of refill,
    // clamped to burst capacity = 3.
    rt.OnTickAdvanced(2, 500);

    int accepted = 0, rejected = 0;
    for (int i = 0; i < 10; ++i) {
        auto rc = rt.SubmitReplicatedEvent(MakeSfx(1));
        if (rc == audio::AudioResult::Success)        ++accepted;
        else if (rc == audio::AudioResult::RateLimited) ++rejected;
    }
    std::cout << "    after 500ms refill: accepted=" << accepted
              << " rejected=" << rejected << "\n";
    assert(accepted == 3);  // capped at burst
    assert(rejected == 7);

    rt.Shutdown();
}

void TestPerPlayerIsolation() {
    std::cout << "  [one spammer doesn't drain another player's bucket]\n";
    audio::AudioRuntime rt; InitRuntime(rt, /*burst*/ 4, /*perSec*/ 4.0f);
    rt.OnTickAdvanced(1, 1000);

    // Player 100 spams to exhaustion.
    for (int i = 0; i < 10; ++i) {
        rt.SubmitReplicatedEvent(MakeSfx(100));
    }

    // Player 200 should still get its full burst.
    int accepted200 = 0;
    for (int i = 0; i < 4; ++i) {
        if (rt.SubmitReplicatedEvent(MakeSfx(200)) ==
            audio::AudioResult::Success) ++accepted200;
    }
    std::cout << "    player 200 burst-accepted=" << accepted200 << "\n";
    assert(accepted200 == 4);

    audio::AudioRuntime::PerPlayerReplicationStats pp100{}, pp200{};
    rt.GetPerPlayerReplicationStats(100, pp100);
    rt.GetPerPlayerReplicationStats(200, pp200);
    assert(pp100.eventsRateLimited == 6u);
    assert(pp200.eventsRateLimited == 0u);

    rt.Shutdown();
}

void TestPerCategoryIsolation() {
    std::cout << "  [draining SFX doesn't drain Music or Voice]\n";
    audio::AudioRuntime rt; InitRuntime(rt, /*sfxBurst*/ 3, /*sfxPerSec*/ 3.0f);
    rt.OnTickAdvanced(1, 1000);

    // Drain SFX.
    for (int i = 0; i < 10; ++i) rt.SubmitReplicatedEvent(MakeSfx(1));

    // Music default is 5/sec/burst-5 — should still accept up to 5.
    int musicAccepted = 0;
    for (int i = 0; i < 8; ++i) {
        if (rt.SubmitReplicatedEvent(MakeMusic(1)) ==
            audio::AudioResult::Success) ++musicAccepted;
    }
    std::cout << "    after SFX drain, music accepted=" << musicAccepted << "\n";
    assert(musicAccepted == 5);

    rt.Shutdown();
}

void TestUiUnlimited() {
    std::cout << "  [UI category default 0/s = unlimited (no rate-limit drops)]\n";
    audio::AudioRuntime rt; InitRuntime(rt);
    rt.OnTickAdvanced(1, 1000);

    int accepted = 0, rateLimited = 0, queueFull = 0, other = 0;
    for (int i = 0; i < 1000; ++i) {
        audio::AudioEvent ev = audio::AudioEvent::MakePlaySoundAtLocation(
            0xCAFEu, audio::Vec3{});
        ev.playerId = 1;
        ev.category = audio::AudioCategory::UI;
        const auto rc = rt.SubmitReplicatedEvent(ev);
        if      (rc == audio::AudioResult::Success)     ++accepted;
        else if (rc == audio::AudioResult::RateLimited) ++rateLimited;
        else if (rc == audio::AudioResult::QueueFull)   ++queueFull;
        else                                            ++other;
    }
    std::cout << "    UI: accepted=" << accepted
              << " rateLimited=" << rateLimited
              << " queueFull=" << queueFull << "\n";
    // The contract for an unlimited category: the rate limiter never
    // rejects. The bounded SPSC ring may return QueueFull when not
    // drained — that's a separate, intentional backpressure path.
    assert(rateLimited == 0);
    assert(other == 0);
    assert(accepted + queueFull == 1000);
    // Aggregate stats: zero rate-limit drops in any category.
    auto s = rt.GetStats();
    for (size_t i = 0; i < 6; ++i) {
        assert(s.replicationEventsRateLimited[i] == 0u);
    }

    rt.Shutdown();
}

void TestValidatorHook() {
    std::cout << "  [IReplicationValidator drops events with PolicyViolation]\n";
    audio::AudioRuntime rt; InitRuntime(rt, /*burst*/ 100, /*perSec*/ 100.0f);
    rt.OnTickAdvanced(1, 1000);

    BlockOnePlayerValidator v{/*blocked*/ 42};
    rt.SetReplicationValidator(&v);

    // Player 42 is blocked — every event rejected before rate limiter.
    for (int i = 0; i < 10; ++i) {
        auto rc = rt.SubmitReplicatedEvent(MakeSfx(42));
        assert(rc == audio::AudioResult::PolicyViolation);
    }
    // Player 1 isn't blocked — every event accepted.
    for (int i = 0; i < 10; ++i) {
        auto rc = rt.SubmitReplicatedEvent(MakeSfx(1));
        assert(rc == audio::AudioResult::Success);
    }

    auto s = rt.GetStats();
    assert(s.replicationEventsRejectedByValidator == 10u);
    // Importantly: validator-rejected events do NOT count as
    // rate-limited (otherwise the buckets wouldn't be capable of
    // distinguishing policy from flow control).
    assert(s.replicationEventsRateLimited[
        static_cast<size_t>(audio::AudioCategory::SFX)] == 0u);

    // Security property: validator-rejected events from a never-
    // seen-before player MUST NOT consume a slot in the LRU table —
    // otherwise the validator hook becomes its own DoS surface (an
    // attacker could force slot exhaustion just by sending events
    // the validator rejects). So GetPerPlayerReplicationStats(42)
    // returns false: no slot was ever allocated for player 42.
    audio::AudioRuntime::PerPlayerReplicationStats pp42{};
    bool found = rt.GetPerPlayerReplicationStats(42, pp42);
    assert(!found);

    rt.SetReplicationValidator(nullptr);
    rt.Shutdown();
}

void TestDeterministicAcrossRuns() {
    std::cout << "  [identical input timeline produces identical decisions]\n";
    auto run = [](std::vector<audio::AudioResult>& results) {
        audio::AudioRuntime rt; InitRuntime(rt, /*burst*/ 4, /*perSec*/ 4.0f);
        rt.OnTickAdvanced(1, 0);
        for (int i = 0; i < 10; ++i) {
            results.push_back(rt.SubmitReplicatedEvent(MakeSfx(7)));
        }
        rt.OnTickAdvanced(2, 500);  // +500 ms = +2 tokens
        for (int i = 0; i < 10; ++i) {
            results.push_back(rt.SubmitReplicatedEvent(MakeSfx(7)));
        }
        rt.Shutdown();
    };

    std::vector<audio::AudioResult> a, b;
    run(a);
    run(b);
    assert(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        assert(a[i] == b[i]);
    }
    std::cout << "    " << a.size() << " decisions identical across runs\n";
}

void TestVoicePathRateLimited() {
    std::cout << "  [OnVoicePacket gated by Voice category bucket]\n";
    // Default Voice budget: 150/sec, burst 150.
    audio::AudioRuntime rt; InitRuntime(rt);
    rt.OnTickAdvanced(1, 1000);

    const std::array<uint8_t, 16> dummy{};
    int accepted = 0, rateLimited = 0, other = 0;
    for (int i = 0; i < 300; ++i) {
        auto rc = rt.OnVoicePacket(/*pid*/ 99, dummy.data(), dummy.size(),
                                   /*seq*/ static_cast<uint16_t>(i),
                                   /*ts*/ 1000);
        if      (rc == audio::AudioResult::Success)     ++accepted;
        else if (rc == audio::AudioResult::RateLimited) ++rateLimited;
        else                                            ++other;
    }
    std::cout << "    voice burst 300: accepted=" << accepted
              << " rateLimited=" << rateLimited << "\n";
    // Burst capacity is 150; 300 attempts should split ~150/150.
    assert(accepted == 150);
    assert(rateLimited == 150);
    assert(other == 0);

    // Aggregate counter for Voice category records the drops.
    auto s = rt.GetStats();
    assert(s.replicationEventsRateLimited[
        static_cast<size_t>(audio::AudioCategory::Voice)] == 150u);

    rt.Shutdown();
}

void TestNewIdCyclingDefense() {
    std::cout << "  [per-tick new-player budget rejects id-cycling]\n";
    audio::AudioRuntime rt; InitRuntime(rt);
    rt.OnTickAdvanced(1, 1000);

    // Default maxNewPlayersPerTick = 8. Submitting from 20 distinct
    // never-seen-before playerIds in the same tick should admit 8
    // and reject the rest.
    int accepted = 0, rateLimited = 0, other = 0;
    for (int i = 0; i < 20; ++i) {
        auto rc = rt.SubmitReplicatedEvent(
            MakeSfx(static_cast<audio::AudioPlayerId>(1000 + i)));
        if      (rc == audio::AudioResult::Success)     ++accepted;
        else if (rc == audio::AudioResult::RateLimited) ++rateLimited;
        else                                            ++other;
    }
    std::cout << "    20 fresh ids in 1 tick: accepted=" << accepted
              << " rateLimited=" << rateLimited << "\n";
    assert(accepted == 8);
    assert(rateLimited == 12);
    assert(other == 0);

    auto s = rt.GetStats();
    assert(s.replicationEventsRejectedNewIdBudget == 12u);

    // Advancing the tick refreshes the budget.
    rt.OnTickAdvanced(2, 1100);
    accepted = 0; rateLimited = 0;
    for (int i = 0; i < 20; ++i) {
        auto rc = rt.SubmitReplicatedEvent(
            MakeSfx(static_cast<audio::AudioPlayerId>(2000 + i)));
        if      (rc == audio::AudioResult::Success)     ++accepted;
        else if (rc == audio::AudioResult::RateLimited) ++rateLimited;
    }
    std::cout << "    after tick advance: accepted=" << accepted
              << " rateLimited=" << rateLimited << "\n";
    assert(accepted == 8);
    assert(rateLimited == 12);

    // Existing players (already in the table) are NOT subject to
    // the new-id budget on subsequent events.
    for (int i = 0; i < 5; ++i) {
        auto rc = rt.SubmitReplicatedEvent(MakeSfx(1000)); // returning player
        assert(rc == audio::AudioResult::Success);
    }

    rt.Shutdown();
}

// Compute RMS over a render buffer. Used to assert audibility /
// silence in TestSourceEnforcement.
float ComputeRms(const std::vector<float>& buf) {
    if (buf.empty()) return 0.0f;
    double acc = 0.0;
    for (float s : buf) acc += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(acc / static_cast<double>(buf.size())));
}

void TestSourceEnforcement() {
    std::cout << "  [Phase 2.5: Client-source ServerAuthoritative rejected with no audio]\n";
    audio::AudioRuntime rt;
    OfflineBackend* backend = InitRuntime(rt);
    rt.OnTickAdvanced(1, 1000);

    // Register a real PCM sound so audio can actually be rendered if
    // the event were to land on the mixer. This is the audibility
    // check: a rejected event must produce silence; an accepted
    // event must produce audible output.
    constexpr audio::AudioSoundId kSnd = 0xDEED;
    std::vector<float> sine(48000); // 1 second mono
    for (size_t i = 0; i < sine.size(); ++i) {
        sine[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f *
                                    static_cast<float>(i) / 48000.0f);
    }
    rt.RegisterPcmSound(kSnd, sine, /*sampleRate*/ 48000, /*channels*/ 1);

    audio::SoundDefinition def;
    def.soundId      = kSnd;
    def.category     = audio::AudioCategory::SFX;
    def.targetBus    = audio::kBusMaster;
    def.spatialized  = false;            // hear it without listener positioning
    def.attenuation.minDistance = 1.0f;
    def.attenuation.maxDistance = 1000.0f;
    rt.RegisterSoundDefinition(def);

    audio::AudioListener lis;
    lis.position = {0, 0, 0};
    lis.forward  = {0, 0, -1};
    lis.up       = {0, 1, 0};
    rt.SetListener(lis);

    // Drain any startup commands.
    for (int i = 0; i < 5; ++i) rt.Update(1.0f / 60.0f);
    std::vector<float> warmup;
    backend->Render(/*frames*/ 4800, warmup);

    audio::AudioEvent ev = MakeSfx(/*pid*/ 5);
    ev.soundId           = kSnd;
    ev.replicationPolicy = audio::AudioReplicationPolicy::ServerAuthoritative;

    // ---- Phase 2.5 enforcement: Client-sourced ServerAuthoritative -----
    auto rcClient = rt.SubmitReplicatedEvent(ev, audio::ReplicationSource::Client);
    assert(rcClient == audio::AudioResult::PolicyViolation);

    // Tick, then render half a second. If the event leaked through to
    // the mixer despite the rejection, we'd hear the 440 Hz sine.
    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
    std::vector<float> rejectedAudio;
    backend->Render(/*frames*/ 24000, rejectedAudio);
    const float rmsRejected = ComputeRms(rejectedAudio);
    std::cout << "    Client-source rejected: render RMS = " << rmsRejected << "\n";
    assert(rmsRejected < 1.0e-4f);  // audibly silent

    // ---- Same event, Server source: must produce audible output -------
    rt.OnTickAdvanced(2, 2000);
    auto rcServer = rt.SubmitReplicatedEvent(ev, audio::ReplicationSource::Server);
    assert(rcServer == audio::AudioResult::Success);

    for (int i = 0; i < 30; ++i) rt.Update(1.0f / 60.0f);
    std::vector<float> acceptedAudio;
    backend->Render(/*frames*/ 24000, acceptedAudio);
    const float rmsAccepted = ComputeRms(acceptedAudio);
    std::cout << "    Server-source accepted: render RMS = " << rmsAccepted << "\n";
    assert(rmsAccepted > 0.05f);  // clearly audible

    // ---- Counters distinguish protocol enforcement from validator -----
    auto s = rt.GetStats();
    assert(s.replicationPolicyViolations == 1u);
    assert(s.replicationEventsRejectedByValidator == 0u);

    // ---- Unknown source (1-arg overload) stays permissive --------------
    rt.OnTickAdvanced(3, 3000);
    auto rcUnknown = rt.SubmitReplicatedEvent(ev);
    assert(rcUnknown == audio::AudioResult::Success);

    // ---- Client-sourced LocalOnly (legitimate forwarding) goes through -
    audio::AudioEvent local = MakeSfx(5);
    local.soundId           = kSnd;
    local.replicationPolicy = audio::AudioReplicationPolicy::LocalOnly;
    auto rcLocal = rt.SubmitReplicatedEvent(local, audio::ReplicationSource::Client);
    assert(rcLocal == audio::AudioResult::Success);

    // No additional policy violations from the legitimate cases.
    auto s2 = rt.GetStats();
    assert(s2.replicationPolicyViolations == 1u);

    rt.Shutdown();
}

} // namespace

int main() {
    std::cout << "[replication_rate_limit_test]\n";
    TestBurstThenReject();
    TestRefillOverTime();
    TestPerPlayerIsolation();
    TestPerCategoryIsolation();
    TestUiUnlimited();
    TestValidatorHook();
    TestVoicePathRateLimited();
    TestNewIdCyclingDefense();
    TestSourceEnforcement();
    TestDeterministicAcrossRuns();
    std::cout << "[replication_rate_limit_test] PASSED\n";
    return 0;
}
