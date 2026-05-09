// examples/multi_tier_ducking/main.cpp
//
// Two-tier sidechain ducking demonstrating the L4D2-style pattern where
// the player's own gunfire ducks BOTH the music AND the volume of
// remote (teammate) gunfire so the local shot has perceptual primacy.
//
// The single-tier `examples/ducking/` example shows music ducking under
// a sidechain bus. The composition here is exactly the same primitives,
// wired up twice:
//
//   Master
//   ├── Music      (Compressor, sidechain = LocalSfx)   ← music ducks under local action
//   └── SfxAll
//       ├── LocalSfx                                     ← your gun, your footsteps
//       └── RemoteSfx (Compressor, sidechain = LocalSfx) ← teammate gun, ambience
//                                                          ducks under your local action
//
// What the demo prints:
//
//   * Baseline RMS during a remote-only shot (LocalSfx silent)
//   * RMS during overlapping local-shot + remote-shot
//
// We expect to see: when the local gun fires while a remote gun is
// already going, the master RMS drops more than it would if only the
// music were ducking, because the remote-gun bus is also being
// compressed by the local-shot sidechain.
//
// This isn't a new engine feature; it's a worked example showing that
// the bus graph and compressor effect already compose into the
// behavior an FPS audio designer would want, with no engine changes.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/bus.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/events.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace audio;

namespace {

constexpr BusId        kMusic     = 1;
constexpr BusId        kSfxAll    = 2;
constexpr BusId        kLocalSfx  = 3;
constexpr BusId        kRemoteSfx = 4;

constexpr AudioSoundId kSndMusic      = 100;
constexpr AudioSoundId kSndLocalShot  = 101;
constexpr AudioSoundId kSndRemoteShot = 102;

class OfflineBackend final : public IAudioBackend {
public:
    AudioResult Start(const AudioBackendConfig& cfg, IAudioRenderCallback* cb) override {
        cfg_ = cfg; callback_ = cb;
        scratch_.assign(static_cast<size_t>(cfg.bufferSize) * cfg.channels, 0.0f);
        return AudioResult::Success;
    }
    void Stop() override { callback_ = nullptr; }
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "Offline"; }
    void Render(uint32_t frames, std::vector<float>& out) {
        out.clear();
        if (!callback_) return;
        const uint32_t bs = cfg_.bufferSize, ch = cfg_.channels;
        out.reserve(static_cast<size_t>(frames + bs) * ch);
        uint32_t produced = 0;
        while (produced < frames) {
            callback_->OnRender(scratch_.data(), bs, ch);
            const uint32_t take = std::min(bs, frames - produced);
            out.insert(out.end(),
                       scratch_.begin(),
                       scratch_.begin() + take * ch);
            produced += take;
        }
    }
private:
    AudioBackendConfig cfg_{};
    IAudioRenderCallback* callback_ = nullptr;
    std::vector<float> scratch_;
};

std::vector<float> MakeMusicLoop(uint32_t sampleRate) {
    const uint32_t frames = sampleRate;
    std::vector<float> out(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        const float a = std::sin(2.0f * 3.14159265f * 220.0f * t);
        const float b = std::sin(2.0f * 3.14159265f * 330.0f * t);
        out[i] = 0.4f * (a + b) * 0.5f;
    }
    return out;
}

std::vector<float> MakeShot(uint32_t sampleRate, float peak, float decaySeconds) {
    const uint32_t frames = sampleRate * 200 / 1000;     // 200 ms tail
    std::vector<float> out(frames);
    uint32_t lcg = 0x1337u;
    for (uint32_t i = 0; i < frames; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const float n   = (static_cast<float>(lcg >> 8) / 16777216.0f) * 2.0f - 1.0f;
        const float env = std::exp(-static_cast<float>(i)
                                   / (static_cast<float>(sampleRate) * decaySeconds));
        out[i] = peak * n * env;
    }
    return out;
}

float Rms(const float* d, size_t n) {
    if (n == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += double(d[i]) * d[i];
    return static_cast<float>(std::sqrt(acc / n));
}

float DbFromAmp(float amp) {
    return amp > 1e-10f ? 20.0f * std::log10(amp) : -120.0f;
}

} // namespace

int main() {
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kBufferSize = 256;
    constexpr uint32_t kChannels   = 2;

    AudioConfig cfg;
    cfg.sampleRate = kSampleRate;
    cfg.bufferSize = kBufferSize;
    cfg.outputMode = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters          = 16;
    cfg.budget.maxRegisteredSounds        = 8;
    cfg.budget.maxVoiceSources            = 0;
    cfg.budget.maxOcclusionChecksPerFrame = 16;

    auto& g = cfg.busGraph;
    g.busCount = 5;

    // Master.
    g.buses[0].id           = kBusMaster;
    g.buses[0].parent       = kBusMaster;
    g.buses[0].outputGainDb = 0.0f;
    std::strncpy(g.buses[0].debugName, "Master", sizeof(g.buses[0].debugName) - 1);

    // Music — ducks under LocalSfx (the standard music-ducks-on-action pattern).
    g.buses[1].id           = kMusic;
    g.buses[1].parent       = kBusMaster;
    g.buses[1].outputGainDb = 0.0f;
    std::strncpy(g.buses[1].debugName, "Music", sizeof(g.buses[1].debugName) - 1);
    g.buses[1].effectCount = 1;
    g.buses[1].effects[0].kind                   = EffectKind::Compressor;
    g.buses[1].effects[0].compressorThresholdDb  = -30.0f;
    g.buses[1].effects[0].compressorRatio        = 8.0f;
    g.buses[1].effects[0].compressorAttackMs     = 5.0f;
    g.buses[1].effects[0].compressorReleaseMs    = 250.0f;
    g.buses[1].effects[0].compressorMakeupDb     = 0.0f;
    g.buses[1].effects[0].compressorSidechainBus = kLocalSfx;

    // SfxAll — pure routing parent for the two SFX tiers. No effect.
    g.buses[2].id           = kSfxAll;
    g.buses[2].parent       = kBusMaster;
    g.buses[2].outputGainDb = 0.0f;
    std::strncpy(g.buses[2].debugName, "SfxAll", sizeof(g.buses[2].debugName) - 1);

    // LocalSfx — your gunfire, footsteps, etc. No effect; this is the
    // sidechain source bus for both Music and RemoteSfx ducking.
    g.buses[3].id           = kLocalSfx;
    g.buses[3].parent       = kSfxAll;
    g.buses[3].outputGainDb = 0.0f;
    std::strncpy(g.buses[3].debugName, "LocalSfx", sizeof(g.buses[3].debugName) - 1);

    // RemoteSfx — teammate gunfire, ambience. Compressed under
    // LocalSfx so when YOU shoot, their gunfire and the ambient
    // chatter dip momentarily so your shot has presence.
    //
    // Less aggressive than the music duck: we want remote SFX to
    // step back ~6-8 dB rather than disappear; music can drop more.
    g.buses[4].id           = kRemoteSfx;
    g.buses[4].parent       = kSfxAll;
    g.buses[4].outputGainDb = 0.0f;
    std::strncpy(g.buses[4].debugName, "RemoteSfx", sizeof(g.buses[4].debugName) - 1);
    g.buses[4].effectCount  = 1;
    g.buses[4].effects[0].kind                   = EffectKind::Compressor;
    g.buses[4].effects[0].compressorThresholdDb  = -28.0f;
    g.buses[4].effects[0].compressorRatio        = 4.0f;
    g.buses[4].effects[0].compressorAttackMs     = 3.0f;
    g.buses[4].effects[0].compressorReleaseMs    = 180.0f;
    g.buses[4].effects[0].compressorMakeupDb     = 0.0f;
    g.buses[4].effects[0].compressorSidechainBus = kLocalSfx;

    AudioRuntime rt;
    auto backendOwn = std::make_unique<OfflineBackend>();
    OfflineBackend* bp = backendOwn.get();
    AudioRuntimeDependencies deps;
    deps.backend = std::move(backendOwn);

    if (rt.Initialize(cfg, std::move(deps)) != AudioResult::Success) {
        std::fprintf(stderr, "Initialize failed\n");
        return 1;
    }

    // ---- Sounds ------------------------------------------------------
    auto musicBuf      = MakeMusicLoop(kSampleRate);
    auto localShotBuf  = MakeShot(kSampleRate, 0.95f, 0.04f);
    auto remoteShotBuf = MakeShot(kSampleRate, 0.55f, 0.05f);

    rt.RegisterPcmSound(kSndMusic,      musicBuf,      kSampleRate, 1);
    rt.RegisterPcmSound(kSndLocalShot,  localShotBuf,  kSampleRate, 1);
    rt.RegisterPcmSound(kSndRemoteShot, remoteShotBuf, kSampleRate, 1);

    auto regDef = [&](AudioSoundId id, BusId bus, bool looping) {
        SoundDefinition d;
        d.soundId       = id;
        d.category      = AudioCategory::SFX;
        d.targetBus     = bus;
        d.spatialized   = false;
        d.looping       = looping;
        d.attenuation.minDistance = 1.0f;
        d.attenuation.maxDistance = 100.0f;
        d.attenuation.volumeFloor = 1.0f;
        rt.RegisterSoundDefinition(d);
    };
    regDef(kSndMusic,      kMusic,     true);
    regDef(kSndLocalShot,  kLocalSfx,  false);
    regDef(kSndRemoteShot, kRemoteSfx, false);

    // Music plays the whole time.
    EmitterDescriptor ed;
    ed.soundId       = kSndMusic;
    ed.position      = {0.0f, 0.0f, 0.0f};
    ed.targetBus     = kMusic;
    ed.category      = AudioCategory::SFX;
    ed.priority      = AudioPriority::Normal;
    ed.isLooping     = true;
    ed.isSpatialized = false;
    ed.attenuation.minDistance = 1.0f;
    ed.attenuation.maxDistance = 100.0f;
    ed.attenuation.volumeFloor = 1.0f;
    auto musicEmitter = rt.CreateEmitter(ed);
    if (!musicEmitter) { std::fprintf(stderr, "music create failed\n"); return 1; }

    auto playOneShot = [&](AudioSoundId id) {
        AudioEvent ev;
        ev.type     = AudioEventType::PlaySoundAtLocation;
        ev.soundId  = id;
        ev.position = {0.0f, 0.0f, 0.0f};
        ev.priority = AudioPriority::High;
        rt.SubmitEvent(ev);
    };

    // Tick-by-tick measurement so the duck dynamics are visible in
    // time. Each tick is 25 ms; we print RMS for each. That shows the
    // attack on the shot transient and the release back to baseline.
    std::vector<float> render;
    auto tick = [&](const char* label) {
        rt.Update(0.025f);
        bp->Render(kSampleRate / 40, render);
        const float r = Rms(render.data(), render.size());
        std::printf("  %-22s rms=%.4f  (%.2f dB)\n", label, r, DbFromAmp(r));
    };

    std::printf("Multi-tier ducking demonstration:\n\n");

    std::printf("Phase 1: music only (baseline)\n");
    for (int i = 0; i < 6; ++i) tick("baseline");

    std::printf("\nPhase 2: remote shot fires alone\n");
    playOneShot(kSndRemoteShot);
    for (int i = 0; i < 14; ++i) {
        char t[24];
        std::snprintf(t, sizeof(t), "+%dms", i * 25);
        tick(t);
    }

    std::printf("\nPhase 3: local shot fires alone\n");
    playOneShot(kSndLocalShot);
    for (int i = 0; i < 14; ++i) {
        char t[24];
        std::snprintf(t, sizeof(t), "+%dms", i * 25);
        tick(t);
    }

    std::printf("\nPhase 4: remote starts, local fires 75 ms later\n");
    playOneShot(kSndRemoteShot);
    for (int i = 0; i < 3; ++i) {
        char t[24];
        std::snprintf(t, sizeof(t), "+%dms (R)", i * 25);
        tick(t);
    }
    playOneShot(kSndLocalShot);
    for (int i = 0; i < 12; ++i) {
        char t[24];
        std::snprintf(t, sizeof(t), "+%dms (L+R)", i * 25);
        tick(t);
    }

    std::printf("\nPhase 5: recovery\n");
    for (int i = 0; i < 6; ++i) tick("recovery");

    rt.Shutdown();
    std::printf("\nWhat to look for:\n");
    std::printf("  Phase 2: remote shot adds energy on top of music; RMS rises briefly.\n");
    std::printf("  Phase 3: local shot drives both compressors; music ducks; RMS dips.\n");
    std::printf("  Phase 4: when local fires while remote is still ringing, the remote\n");
    std::printf("           bus is compressed (in addition to music). The local shot\n");
    std::printf("           dominates the master mix; the remote is pushed under.\n");
    return 0;
}
