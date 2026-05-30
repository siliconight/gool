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

// tests/unit/voice_mute_budget_test.cpp
//
// Verifies the v0.13.0 voice-control APIs:
//
//   - 2.4: SetVoiceSourceMuted / SetVoiceSourceVolume / Get* round trips
//   - 2.4: muted source's packets don't reach the PCM ring; counter rises
//   - 2.6: SetVoiceBandwidthBudget + SuggestVoiceBitrate ladder behavior
//   - 2.6: ReportVoiceBytesSent drains the bucket; downgrade/drop counters
//
// Uses the same OfflineBackend pattern as voice_telemetry_test.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

class OfflineBackend final : public IAudioBackend {
public:
    AudioResult Start(const AudioBackendConfig& cfg, IAudioRenderCallback* cb) override {
        cfg_ = cfg; cb_ = cb;
        scratch_.assign(static_cast<size_t>(cfg.bufferSize) * cfg.channels, 0.0f);
        return AudioResult::Success;
    }
    void Stop() override { cb_ = nullptr; }
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "Offline"; }
    void Render(uint32_t frames) {
        if (!cb_) return;
        const uint32_t bs = cfg_.bufferSize;
        uint32_t produced = 0;
        while (produced < frames) {
            cb_->OnRender(scratch_.data(), bs, cfg_.channels);
            produced += bs;
        }
    }
private:
    AudioBackendConfig cfg_{};
    IAudioRenderCallback* cb_ = nullptr;
    std::vector<float> scratch_;
};

constexpr uint16_t kPayloadBytes = 320;
void StampPayload(uint8_t* dst, uint16_t seq) {
    std::memset(dst, 0, kPayloadBytes);
    std::memcpy(dst, &seq, sizeof(seq));
}

// Each test creates its own runtime inline — no shared factory.

// Forward declaration of the host-side packet-bytes estimator (defined
// after TestBandwidthBudgetLadder for the actual implementation).
uint32_t EstimatedPacketBytes(int32_t br);

// ---- 2.4 Mute/volume API surface -----------------------------------------

void TestMuteVolumeRoundTrip() {
    std::printf("[TestMuteVolumeRoundTrip]\n");

    AudioConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    cfg.outputMode = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters = 4;
    cfg.budget.maxRegisteredSounds = 4;
    cfg.budget.maxStreamingAssets = 1;
    cfg.budget.maxStreamingVoices = 1;
    cfg.budget.maxVoiceSources = 4;
    cfg.enableVoice = true;
    cfg.enableOcclusion = false;
    cfg.voiceMaxPacketBytes = 1024;
    cfg.voicePacketRingDepth = 64;

    AudioRuntime rt;
    AudioRuntimeDependencies deps;
    deps.backend = std::make_unique<OfflineBackend>();
    EXPECT(rt.Initialize(cfg, std::move(deps)) == AudioResult::Success);

    constexpr AudioPlayerId kP = 42;
    EXPECT(static_cast<bool>(rt.RegisterVoiceSource(kP)));

    // Initial state: not muted, full volume.
    bool isMuted = true;
    EXPECT(rt.IsVoiceSourceMuted(kP, isMuted));
    EXPECT(isMuted == false);

    float vol = 0.0f;
    EXPECT(rt.GetVoiceSourceVolume(kP, vol));
    EXPECT(vol == 1.0f);

    // Mute, query, unmute, query.
    EXPECT(rt.SetVoiceSourceMuted(kP, true) == AudioResult::Success);
    EXPECT(rt.IsVoiceSourceMuted(kP, isMuted));
    EXPECT(isMuted == true);
    EXPECT(rt.SetVoiceSourceMuted(kP, false) == AudioResult::Success);
    EXPECT(rt.IsVoiceSourceMuted(kP, isMuted));
    EXPECT(isMuted == false);

    // Set volume, query.
    EXPECT(rt.SetVoiceSourceVolume(kP, 0.5f) == AudioResult::Success);
    EXPECT(rt.GetVoiceSourceVolume(kP, vol));
    EXPECT(vol == 0.5f);

    // Boundary values.
    EXPECT(rt.SetVoiceSourceVolume(kP, 0.0f) == AudioResult::Success);
    EXPECT(rt.SetVoiceSourceVolume(kP, 4.0f) == AudioResult::Success);
    // Out of range — rejected without state change.
    EXPECT(rt.SetVoiceSourceVolume(kP, -0.1f) == AudioResult::InvalidArgument);
    EXPECT(rt.SetVoiceSourceVolume(kP, 4.5f) == AudioResult::InvalidArgument);

    // Unregistered player: InvalidHandle on Set, false on Get.
    EXPECT(rt.SetVoiceSourceMuted(999, true) == AudioResult::InvalidHandle);
    EXPECT(rt.SetVoiceSourceVolume(999, 0.5f) == AudioResult::InvalidHandle);
    EXPECT(rt.IsVoiceSourceMuted(999, isMuted) == false);
    EXPECT(rt.GetVoiceSourceVolume(999, vol)   == false);

    rt.Shutdown();
}

// ---- 2.4 Mute drops decode work ------------------------------------------

void TestMutedSourceDropsFrames() {
    std::printf("[TestMutedSourceDropsFrames]\n");

    AudioConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    cfg.outputMode = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters = 4;
    cfg.budget.maxRegisteredSounds = 4;
    cfg.budget.maxStreamingAssets = 1;
    cfg.budget.maxStreamingVoices = 1;
    cfg.budget.maxVoiceSources = 4;
    cfg.enableVoice = true;
    cfg.enableOcclusion = false;
    cfg.voiceMaxPacketBytes = 1024;
    cfg.voicePacketRingDepth = 64;

    AudioRuntime rt;
    auto backend = std::make_unique<OfflineBackend>();
    OfflineBackend* bp = backend.get();
    AudioRuntimeDependencies deps;
    deps.backend = std::move(backend);
    EXPECT(rt.Initialize(cfg, std::move(deps)) == AudioResult::Success);

    constexpr AudioPlayerId kP = 5;
    EXPECT(static_cast<bool>(rt.RegisterVoiceSource(kP)));
    EXPECT(rt.SetVoiceSourceMuted(kP, true) == AudioResult::Success);

    // Submit 50 packets to the muted source.
    uint8_t payload[kPayloadBytes];
    TimestampMs ts = 1000;
    for (uint32_t i = 0; i < 50; ++i) {
        const uint16_t seq = static_cast<uint16_t>(1000 + i);
        StampPayload(payload, seq);
        rt.OnVoicePacket(kP, payload, kPayloadBytes, seq, ts, ts);
        ts += 20;
    }

    // Drive a few render callbacks to advance ticks.
    for (int i = 0; i < 10; ++i) {
        rt.Update(0.02f);
        bp->Render(256);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Final tick to flush the mute drains.
    rt.Update(0.02f);

    const auto stats = rt.GetStats();
    // Counter should have incremented — exact value depends on how many
    // packets were drained before tick ended; expect >0.
    std::printf("  voiceFramesDroppedDueToMute = %llu (any-positive expected)\n",
                static_cast<unsigned long long>(stats.voiceFramesDroppedDueToMute));
    EXPECT(stats.voiceFramesDroppedDueToMute > 0);

    rt.Shutdown();
}

// ---- 2.6 Bandwidth budget ------------------------------------------------
//
// The token bucket is sized so capacity == 1 second of budget (we don't
// allow burst beyond that). For a 20ms frame, the per-frame estimated
// bytes at each rung are:
//   32 kbps: 80 + 12 (hdr) = 92 B
//   24 kbps: 60 + 12 (hdr) = 72 B
//   16 kbps: 40 + 12 (hdr) = 52 B
//
// SuggestVoiceBitrate picks the highest rung whose cost is <= bucket
// tokens. By choosing budgets that land cleanly between those rung
// costs, we get deterministic behavior on the very first call (no
// timing dependency). Each scenario tests one rung selection.

void TestBandwidthBudgetLadder() {
    std::printf("[TestBandwidthBudgetLadder]\n");

    AudioConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    cfg.outputMode = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters = 4;
    cfg.budget.maxRegisteredSounds = 4;
    cfg.budget.maxStreamingAssets = 1;
    cfg.budget.maxStreamingVoices = 1;
    cfg.budget.maxVoiceSources = 4;
    cfg.enableVoice = true;
    cfg.enableOcclusion = false;

    AudioRuntime rt;
    AudioRuntimeDependencies deps;
    deps.backend = std::make_unique<OfflineBackend>();
    EXPECT(rt.Initialize(cfg, std::move(deps)) == AudioResult::Success);

    constexpr AudioPlayerId kP = 100;
    EXPECT(static_cast<bool>(rt.RegisterVoiceSource(kP)));

    // (a) No budget set → default 32 kbps regardless of bucket.
    EXPECT(rt.SuggestVoiceBitrate(kP, 20) == 32000);

    // (b) Budget = 80 B/sec → initial bucket 80 tokens. 32k (92) doesn't
    //     fit, 24k (72) does, 16k (52) also does. Returns 24000.
    //     SetBandwidthBudget resets bucket state, so we get a clean
    //     initial cap each time.
    EXPECT(rt.SetVoiceBandwidthBudget(kP, 80) == AudioResult::Success);
    EXPECT(rt.SuggestVoiceBitrate(kP, 20) == 24000);

    // (c) Budget = 60 B/sec → cap 60. 32k (92) fails, 24k (72) fails,
    //     16k (52) fits. Returns 16000.
    EXPECT(rt.SetVoiceBandwidthBudget(kP, 60) == AudioResult::Success);
    EXPECT(rt.SuggestVoiceBitrate(kP, 20) == 16000);

    // (d) Budget = 40 B/sec → cap 40. No rung fits. Returns 0 (drop).
    EXPECT(rt.SetVoiceBandwidthBudget(kP, 40) == AudioResult::Success);
    EXPECT(rt.SuggestVoiceBitrate(kP, 20) == 0);

    // (e) ReportVoiceBytesSent at a downgraded rung bumps the
    //     framesBudgetDowngraded counter. Use scenario (b) again.
    EXPECT(rt.SetVoiceBandwidthBudget(kP, 80) == AudioResult::Success);
    const int32_t br = rt.SuggestVoiceBitrate(kP, 20);
    EXPECT(br == 24000);
    EXPECT(rt.ReportVoiceBytesSent(kP, EstimatedPacketBytes(br), br)
            == AudioResult::Success);

    const auto stats = rt.GetStats();
    std::printf("  voiceBytesSent             = %llu\n",
                static_cast<unsigned long long>(stats.voiceBytesSent));
    std::printf("  voiceFramesBudgetDowngraded= %llu\n",
                static_cast<unsigned long long>(stats.voiceFramesBudgetDowngraded));
    std::printf("  voiceFramesBudgetDropped   = %llu\n",
                static_cast<unsigned long long>(stats.voiceFramesBudgetDropped));
    EXPECT(stats.voiceBytesSent              > 0);
    EXPECT(stats.voiceFramesBudgetDowngraded > 0);
    // Scenario (d) hit Suggest==0 once → at least one drop recorded.
    EXPECT(stats.voiceFramesBudgetDropped    > 0);

    rt.Shutdown();
}

// Helper for the budget test — mirror the engine's estimator so tests
// stay realistic about what they're spending.
uint32_t EstimatedPacketBytes(int32_t br) {
    // Mirrors VoiceSourceManager's namespace { EstimatePacketBytes } —
    // 20ms frame, 12-byte header overhead.
    if (br <= 0) return 0;
    const uint64_t bits = static_cast<uint64_t>(br) * 20ull / 1000ull;
    return static_cast<uint32_t>(bits / 8ull) + 12u;
}

void TestBandwidthBudgetExhaustion() {
    std::printf("[TestBandwidthBudgetExhaustion]\n");

    AudioConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    cfg.outputMode = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters = 4;
    cfg.budget.maxRegisteredSounds = 4;
    cfg.budget.maxStreamingAssets = 1;
    cfg.budget.maxStreamingVoices = 1;
    cfg.budget.maxVoiceSources = 4;
    cfg.enableVoice = true;
    cfg.enableOcclusion = false;

    AudioRuntime rt;
    AudioRuntimeDependencies deps;
    deps.backend = std::make_unique<OfflineBackend>();
    EXPECT(rt.Initialize(cfg, std::move(deps)) == AudioResult::Success);

    constexpr AudioPlayerId kP = 200;
    EXPECT(static_cast<bool>(rt.RegisterVoiceSource(kP)));

    // Budget set TOO low for even 16 kbps. 16 kbps at 20ms = 52 B/packet.
    // 50 packets/sec * 52 B = 2600 B/sec needed. Set to 100 B/sec.
    EXPECT(rt.SetVoiceBandwidthBudget(kP, 100) == AudioResult::Success);

    // Call Suggest 20 times rapid-fire — bucket can't refill, drops dominate.
    int drops = 0;
    for (int i = 0; i < 20; ++i) {
        const int32_t br = rt.SuggestVoiceBitrate(kP, 20);
        if (br == 0) ++drops;
        else rt.ReportVoiceBytesSent(kP, EstimatedPacketBytes(br), br);
    }
    EXPECT(drops > 10);
    std::printf("  rapid-fire drops: %d / 20\n", drops);

    const auto stats = rt.GetStats();
    EXPECT(stats.voiceFramesBudgetDropped > 0);

    rt.Shutdown();
}

} // namespace

int main() {
    TestMuteVolumeRoundTrip();
    TestMutedSourceDropsFrames();
    TestBandwidthBudgetLadder();
    TestBandwidthBudgetExhaustion();

    if (gFails == 0) {
        std::printf("[voice_mute_budget_test] PASSED\n");
        return 0;
    }
    std::printf("[voice_mute_budget_test] %d failure(s)\n", gFails);
    return 1;
}
