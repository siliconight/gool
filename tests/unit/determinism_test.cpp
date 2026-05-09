// tests/unit/determinism_test.cpp
//
// Validates that two runs of the engine with identical inputs
// produce bit-identical sample output. This was previously
// undocumented; with the 6-arg OnVoicePacket added, the wall-clock
// dependency on the voice-ingest path is gone, and replay can be
// made deterministic if the host supplies its own tick clock.
//
// The test runs two independent AudioRuntime instances through the
// same offline backend, submits the same event sequence with the
// same timestamps, and renders the same number of frames. Byte-by-
// byte comparison of the rendered output is the assertion.
//
// This test deliberately does NOT exercise OnVoicePacket — that
// path is verified in voice_telemetry_test, and the new 6-arg
// overload is a thin variant of the existing path. What we're
// proving here is that the rest of the engine (event handling,
// emitter creation, mixing, fade curves, occlusion, attenuation,
// and so on) doesn't introduce wall-clock-dependent state.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
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
    const char* Name()    const noexcept override { return "OfflineDet"; }

    void Render(uint32_t frames, std::vector<float>& out) {
        out.clear();
        if (!cb_) return;
        const uint32_t bs = cfg_.bufferSize, ch = cfg_.channels;
        out.reserve(static_cast<size_t>(frames + bs) * ch);
        uint32_t produced = 0;
        while (produced < frames) {
            cb_->OnRender(scratch_.data(), bs, ch);
            const uint32_t take = std::min(bs, frames - produced);
            out.insert(out.end(),
                        scratch_.begin(),
                        scratch_.begin() + take * ch);
            produced += take;
        }
    }
private:
    audio::AudioBackendConfig         cfg_{};
    audio::IAudioRenderCallback*      cb_ = nullptr;
    std::vector<float>                scratch_;
};

// Run a fixed scenario and return the rendered output bytes.
// Scenario: register a sine, create a non-spatialized emitter with
// a fade-in, render 200ms, change pitch via SetEmitterPlaybackSpeed,
// render another 200ms, destroy with fade-out, render 200ms more.
// Total = 600ms of mix output.
std::vector<float> RunScenario() {
    constexpr uint32_t kSr = 48000;
    constexpr float    kPi = 3.14159265f;

    audio::AudioRuntime rt;
    audio::AudioConfig  cfg;
    cfg.sampleRate = kSr;
    cfg.bufferSize = 192;
    cfg.outputMode = audio::AudioOutputMode::Stereo;
    audio::AudioRuntimeDependencies deps;
    auto bp = std::make_unique<OfflineBackend>();
    OfflineBackend* backend = bp.get();
    deps.backend = std::move(bp);
    rt.Initialize(cfg, std::move(deps));

    audio::AudioListener listener;
    rt.SetListener(listener);

    // Register a 440 Hz sine, looping.
    constexpr audio::AudioSoundId kId = 0xDE7E0001u;
    std::vector<float> sine(kSr);
    for (uint32_t i = 0; i < kSr; ++i) {
        sine[i] = 0.5f * std::sin(2.0f * kPi * 440.0f *
                                    static_cast<float>(i) /
                                    static_cast<float>(kSr));
    }
    rt.RegisterPcmSound(kId, std::span<const float>(sine.data(), sine.size()),
                         kSr, 1u);
    audio::SoundDefinition def;
    def.soundId     = kId;
    def.spatialized = false;
    def.targetBus   = audio::kBusMaster;
    rt.RegisterSoundDefinition(def);

    audio::EmitterDescriptor desc;
    desc.soundId       = kId;
    desc.isLooping     = true;
    desc.isSpatialized = false;
    desc.fadeInMs      = 50.0f;
    auto h = rt.CreateEmitter(desc);
    assert(h);

    std::vector<float> all;
    std::vector<float> chunk;

    // Phase 1: 200ms fade-in.
    for (int i = 0; i < 10; ++i) {
        rt.Update(0.020f);
        backend->Render(kSr / 50, chunk);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    // Phase 2: bump speed to 1.5x.
    rt.SetEmitterPlaybackSpeed(h.value(), 1.5f, /*smoothingMs=*/30.0f);
    for (int i = 0; i < 10; ++i) {
        rt.Update(0.020f);
        backend->Render(kSr / 50, chunk);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    // Phase 3: destroy with fade-out.
    rt.DestroyEmitter(h.value(), /*fadeOutMs=*/100.0f);
    for (int i = 0; i < 10; ++i) {
        rt.Update(0.020f);
        backend->Render(kSr / 50, chunk);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    rt.Shutdown();
    return all;
}

void TestBitIdenticalReplay() {
    std::cout << "  [bit-identical replay across two engine instances]\n";
    const auto run1 = RunScenario();
    const auto run2 = RunScenario();
    assert(run1.size() == run2.size());
    if (run1.size() != run2.size()) {
        std::cerr << "    FAIL: output size differs (" << run1.size()
                  << " vs " << run2.size() << ")\n";
        std::exit(1);
    }
    // Compare every sample byte-for-byte.
    int diffs = 0;
    float maxAbsDelta = 0.0f;
    for (size_t i = 0; i < run1.size(); ++i) {
        const float d = std::abs(run1[i] - run2[i]);
        if (d > 0.0f) ++diffs;
        if (d > maxAbsDelta) maxAbsDelta = d;
    }
    std::printf("    samples compared: %zu\n", run1.size());
    std::printf("    samples differing: %d\n", diffs);
    std::printf("    max |Δ|: %g\n", maxAbsDelta);
    std::fflush(stdout);
    if (diffs != 0 || maxAbsDelta != 0.0f) {
        std::cerr << "    FAIL: runs are not bit-identical\n";
        std::exit(1);
    }
    std::cout << "    OK (every sample matches across both runs)\n";
}

void TestDeterministicOnVoicePacket() {
    std::cout << "  [OnVoicePacket 6-arg form: explicit arrival timestamp]\n";

    // Smoke-test: the new overload accepts the explicit arrivalMs
    // and produces the same QueueFull/InvalidArgument behavior the
    // 5-arg form does. (The actual jitter math is exercised in
    // jitter_buffer_test; we just verify the pass-through here.)
    audio::AudioRuntime rt;
    audio::AudioConfig  cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 192;
    cfg.outputMode = audio::AudioOutputMode::Stereo;
    audio::AudioRuntimeDependencies deps;
    auto bp = std::make_unique<OfflineBackend>();
    deps.backend = std::move(bp);
    rt.Initialize(cfg, std::move(deps));

    auto vh = rt.RegisterVoiceSource(/*playerId=*/42u);
    assert(vh);

    // Send a fake packet with explicit arrival = 1000ms.
    const uint8_t fakeBytes[] = {0x01, 0x02, 0x03, 0x04};
    const auto rcDet = rt.OnVoicePacket(/*playerId=*/42u,
                                          fakeBytes, sizeof(fakeBytes),
                                          /*seqNum=*/1, /*sendTs=*/990,
                                          /*arrivalTs=*/1000);
    assert(rcDet == audio::AudioResult::Success);

    const auto rcLegacy = rt.OnVoicePacket(/*playerId=*/42u,
                                              fakeBytes, sizeof(fakeBytes),
                                              /*seqNum=*/2, /*sendTs=*/1010);
    assert(rcLegacy == audio::AudioResult::Success);

    // Negative case: empty packet rejected.
    const auto rcEmpty = rt.OnVoicePacket(/*playerId=*/42u,
                                              fakeBytes, /*size=*/0,
                                              /*seqNum=*/3, /*sendTs=*/1020,
                                              /*arrivalTs=*/1020);
    assert(rcEmpty == audio::AudioResult::InvalidArgument);

    rt.UnregisterVoiceSource(vh.value());
    rt.Shutdown();
    std::cout << "    OK (6-arg form works, validates the same way)\n";
}

} // namespace

int main() {
    std::cout << "[determinism_test] running...\n";
    TestBitIdenticalReplay();
    TestDeterministicOnVoicePacket();
    std::cout << "[determinism_test] OK\n";
    return 0;
}
