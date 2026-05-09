// examples/ducking/main.cpp
//
// End-to-end demonstration of the bus graph + sidechain compressor. Builds:
//
//   Master
//   ├── Music     (Compressor, sidechain = LocalGun) ← plays a sine pad
//   └── LocalGun  (silent: drives sidechain only)    ← plays a noise burst
//
// The demo uses an OfflineBackend: instead of spinning a real audio thread
// and racing a sleep against the device clock, it stashes the mixer's
// IAudioRenderCallback and lets the main thread drive OnRender directly.
// That makes every frame deterministic; no truncation, no jitter; so the
// printed RMS / dB column is an exact measurement of what the engine
// produces.
//
// The expected output: baseline RMS reflects the music alone; once the
// gunshot fires the sidechain compressor on Music drops the music's gain
// substantially, the RMS column dips, and recovers as the compressor
// releases. (LocalGun is muted in this demo so master contains only the
// ducked Music; making the duck unambiguous in the master RMS.)

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
#include <vector>

using namespace audio;

namespace {

constexpr BusId        kMusic    = 1;
constexpr BusId        kLocalGun = 2;
constexpr AudioSoundId kSndMusic = 100;
constexpr AudioSoundId kSndShot  = 101;

// ---------------------------------------------------------------------------
// OfflineBackend; synchronous, deterministic, single-threaded.
//
// Start() stashes the callback. The main thread drives OnRender by calling
// Render(frames). This is the cleanest substrate for a measurement demo:
// every output sample is the engine's exact output for an exact tick of
// simulated time.

class OfflineBackend final : public IAudioBackend {
public:
    AudioResult Start(const AudioBackendConfig& cfg, IAudioRenderCallback* cb) override {
        cfg_      = cfg;
        callback_ = cb;
        scratch_.assign(static_cast<size_t>(cfg.bufferSize) * cfg.channels, 0.0f);
        return AudioResult::Success;
    }
    void Stop() override { callback_ = nullptr; }

    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "Offline"; }

    // Render `frames` frames into `out` (cleared first). Issues as many
    // bufferSize-sized callbacks as needed.
    void Render(uint32_t frames, std::vector<float>& out) {
        out.clear();
        if (!callback_) return;
        const uint32_t bs = cfg_.bufferSize;
        const uint32_t ch = cfg_.channels;
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
    AudioBackendConfig    cfg_{};
    IAudioRenderCallback* callback_ = nullptr;
    std::vector<float>    scratch_;
};

// ---------------------------------------------------------------------------
// Tone generators

std::vector<float> MakeMusicLoop(uint32_t sampleRate) {
    // 1-second mono pad: stacked 220 Hz + 330 Hz sines at moderate level.
    const uint32_t frames = sampleRate;
    std::vector<float> out(frames, 0.0f);
    for (uint32_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
        const float a = std::sin(2.0f * 3.14159265f * 220.0f * t);
        const float b = std::sin(2.0f * 3.14159265f * 330.0f * t);
        out[i] = 0.4f * (a + b) * 0.5f;
    }
    return out;
}

std::vector<float> MakeGunshot(uint32_t sampleRate) {
    // ~150 ms exponentially-decaying noise burst, peak ~0.95 → 0 dBFS-ish so
    // the sidechain detector sees plenty of energy above threshold.
    const uint32_t frames = sampleRate * 150 / 1000;
    std::vector<float> out(frames, 0.0f);
    uint32_t lcg = 0x1337u;
    for (uint32_t i = 0; i < frames; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const float n   = (static_cast<float>(lcg >> 8) / 16777216.0f) * 2.0f - 1.0f;
        const float env = std::exp(-static_cast<float>(i)
                                   / (static_cast<float>(sampleRate) * 0.04f));
        out[i] = 0.95f * n * env;
    }
    return out;
}

float Rms(const float* data, size_t samples) {
    if (samples == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < samples; ++i) acc += static_cast<double>(data[i]) * data[i];
    return static_cast<float>(std::sqrt(acc / static_cast<double>(samples)));
}

float DbFromAmp(float amp) {
    return amp > 1e-10f ? 20.0f * std::log10(amp) : -120.0f;
}

} // namespace

int main() {
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kBufferSize = 256;
    constexpr uint32_t kChannels   = 2;

    // ---- Bus graph -------------------------------------------------------
    AudioConfig cfg;
    cfg.sampleRate = kSampleRate;
    cfg.bufferSize = kBufferSize;
    cfg.outputMode = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters          = 16;
    cfg.budget.maxRegisteredSounds        = 8;
    cfg.budget.maxVoiceSources            = 0;
    cfg.budget.maxOcclusionChecksPerFrame = 16;

    auto& g = cfg.busGraph;
    g.busCount = 3;

    // Master.
    g.buses[0].id           = kBusMaster;
    g.buses[0].parent       = kBusMaster;
    g.buses[0].outputGainDb = 0.0f;
    std::strncpy(g.buses[0].debugName, "Master", sizeof(g.buses[0].debugName) - 1);

    // Music: compressor sidechained from LocalGun. Aggressive settings so
    // the duck is unambiguous in the printed RMS.
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
    g.buses[1].effects[0].compressorSidechainBus = kLocalGun;

    // LocalGun: silent (sidechain-only) so master contains *only* the ducked
    // music. In a real game LocalGun would also be audible.
    g.buses[2].id           = kLocalGun;
    g.buses[2].parent       = kBusMaster;
    g.buses[2].outputGainDb = 0.0f;
    g.buses[2].silent       = true;
    std::strncpy(g.buses[2].debugName, "LocalGun", sizeof(g.buses[2].debugName) - 1);

    // ---- Initialize ------------------------------------------------------
    auto backend = std::make_unique<OfflineBackend>();
    OfflineBackend* bp = backend.get();

    AudioRuntime rt;
    AudioRuntimeDependencies deps;
    deps.backend = std::move(backend);

    if (rt.Initialize(cfg, std::move(deps)) != AudioResult::Success) {
        std::fprintf(stderr, "Initialize failed\n");
        return 1;
    }

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    rt.SetListener(lis);

    auto musicSamples = MakeMusicLoop(kSampleRate);
    auto shotSamples  = MakeGunshot(kSampleRate);

    SoundDefinition md;
    md.soundId     = kSndMusic;
    md.category    = AudioCategory::Music;
    md.targetBus   = kMusic;
    md.looping     = true;
    md.spatialized = false;
    rt.RegisterSoundDefinition(md);
    rt.RegisterPcmSound(kSndMusic, std::span{musicSamples}, kSampleRate, 1);

    SoundDefinition sd;
    sd.soundId     = kSndShot;
    sd.category    = AudioCategory::SFX;
    sd.targetBus   = kLocalGun;
    sd.looping     = false;
    sd.spatialized = false;
    rt.RegisterSoundDefinition(sd);
    rt.RegisterPcmSound(kSndShot, std::span{shotSamples}, kSampleRate, 1);

    EmitterDescriptor ed;
    ed.soundId       = kSndMusic;
    ed.targetBus     = kMusic;
    ed.isLooping     = true;
    ed.isSpatialized = false;
    ed.category      = AudioCategory::Music;
    auto h = rt.CreateEmitter(ed);
    if (!h) {
        std::fprintf(stderr, "CreateEmitter failed\n");
        return 1;
    }

    // ---- Drive the engine in fixed 25ms ticks ---------------------------
    constexpr uint32_t kTickMs        = 25;
    const     uint32_t kFramesPerTick = (kSampleRate * kTickMs) / 1000;
    std::vector<float> buf;
    buf.reserve(static_cast<size_t>(kFramesPerTick + kBufferSize) * kChannels);

    auto tick = [&](const char* tag) {
        rt.Update(static_cast<float>(kTickMs) / 1000.0f);
        bp->Render(kFramesPerTick, buf);
        const float rms = Rms(buf.data(), buf.size());
        std::printf("  %-10s rms=%.4f  (%6.2f dB)\n",
                     tag, rms, DbFromAmp(rms));
    };

    std::printf("Baseline (music only, 8 ticks @ 25ms)\n");
    for (int i = 0; i < 8; ++i) tick("baseline");

    std::printf("\nFiring gunshot...\n");
    AudioEvent ev;
    ev.type     = AudioEventType::PlaySoundAtLocation;
    ev.soundId  = kSndShot;
    ev.position = {0.0f, 0.0f, 0.0f};
    ev.priority = AudioPriority::High;
    rt.SubmitEvent(ev);

    // Track the duck through 24 ticks (~600 ms); enough to see the
    // compressor attack on the gunshot transient and release back to
    // baseline after the 150 ms shot ends.
    for (int i = 0; i < 24; ++i) {
        char tag[16];
        std::snprintf(tag, sizeof(tag), "+%dms", i * static_cast<int>(kTickMs));
        tick(tag);
    }

    rt.DestroyEmitter(h.value());
    rt.Shutdown();

    std::printf(
        "\nThe drop in RMS during the gunshot window is the Music bus's\n"
        "sidechain compressor attenuating in response to LocalGun's energy.\n"
        "Recovery follows the configured 250 ms release time.\n");
    return 0;
}
