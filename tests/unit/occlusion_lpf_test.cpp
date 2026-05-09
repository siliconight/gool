// tests/unit/occlusion_lpf_test.cpp
//
// Exercises the per-voice occlusion low-pass filter at two layers:
//
//   1. Mixer-level: drive AudioMixer directly with a 10 kHz sine and a
//      200 Hz sine. With lpfAmount=0 the filter is a no-op; output peak
//      ≈ input peak. With lpfAmount=0.7 (the spatializer's occlusion cap)
//      the cutoff lands ~1.7 kHz, so the 10 kHz tone is heavily damped
//      (18+ dB of biquad rolloff) while the 200 Hz tone is unchanged.
//
//   2. End-to-end: spin up a real AudioRuntime with a custom
//      IAudioGeometryQuery that always reports "occluded". Register a
//      10 kHz sine, play it through an emitter, render via the offline
//      backend, and confirm peak amplitude is substantially lower than
//      with a clear (NullGeometryQuery) line of sight. Proves that the
//      wiring from occlusion → spatializer.lowPassAmount → mixer LPF
//      composes correctly.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/bus.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/events.h"
#include "audio_engine/geometry_query.h"
#include "audio_engine/types.h"

#include "audio_engine/mixer/audio_mixer.h"
#include "audio_engine/mixer/bus_graph.h"
#include "audio_engine/mixer/mixer_command.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kBufferSize = 256;

constexpr float kPi = 3.14159265358979323846f;

std::vector<float> SineMono(uint32_t frames, float hz) {
    std::vector<float> v(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        v[i] = std::sin(2.0f * kPi * hz * static_cast<float>(i)
                                / static_cast<float>(kSampleRate));
    }
    return v;
}

float Peak(const float* d, size_t n) {
    float p = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float a = std::fabs(d[i]);
        if (a > p) p = a;
    }
    return p;
}

// -------------------------------------------------------------------------
// Mixer-level LPF tests; drive AudioMixer + BusGraph directly. Bypass the
// runtime and the spatializer.
// -------------------------------------------------------------------------

struct MixerRig {
    BusGraph    bg;
    AudioMixer  mixer;
    std::vector<float> render;

    MixerRig()
        : bg(),
          mixer(/*maxMixVoices*/ 4, /*outputChannels*/ 2,
                /*commandRingDepth*/ 32, &bg, /*sampleRate*/ kSampleRate),
          render(static_cast<size_t>(kBufferSize) * 2, 0.0f)
    {
        // Empty BusGraphConfig; Build() defaults to a single master-only graph.
        BusGraphConfig cfg{};
        const auto rc = bg.Build(cfg, kSampleRate, /*channels*/ 2, kBufferSize);
        if (rc != AudioResult::Success) ++gFails;
    }

    void StartSineWithLpf(const float* pcm, uint32_t frames,
                           float lpfAmount, float gain = 1.0f) {
        MixerCommand cmd;
        cmd.kind          = MixerCommandKind::StartSound;
        cmd.mixSlot       = 1;
        cmd.gain          = gain;
        cmd.pan           = 0.0f;
        cmd.pitch         = 1.0f;
        cmd.lowPassAmount = lpfAmount;
        cmd.targetBus     = kBusMaster;
        cmd.pcmData       = pcm;
        cmd.pcmFrames     = frames;
        cmd.pcmChannels   = 1;
        cmd.looping       = true;
        mixer.PostCommand(cmd);
    }

    // Render N buffers and accumulate them into one big chunk for analysis.
    std::vector<float> Render(uint32_t buffers) {
        std::vector<float> out(static_cast<size_t>(buffers) * kBufferSize * 2);
        for (uint32_t i = 0; i < buffers; ++i) {
            mixer.OnRender(render.data(), kBufferSize, /*channels*/ 2);
            std::copy(render.begin(), render.end(),
                      out.begin() + static_cast<ptrdiff_t>(i) * kBufferSize * 2);
        }
        return out;
    }
};

void TestMixerLpfBypassedAtZero() {
    MixerRig rig;
    auto src = SineMono(kSampleRate, 10000.0f);    // 10 kHz, 1 s loop
    rig.StartSineWithLpf(src.data(), static_cast<uint32_t>(src.size()),
                          /*lpfAmount*/ 0.0f);
    auto out = rig.Render(/*buffers*/ 8);          // ~ 43 ms

    // Drop the first buffer so we measure steady state.
    const size_t start = kBufferSize * 2;
    const float peak = Peak(out.data() + start, out.size() - start);
    std::printf("  10 kHz, lpfAmount=0.0 → peak=%.3f (expect ~0.7+ on a centered pan)\n", peak);
    EXPECT(peak > 0.6f);     // equal-power center pan attenuates ~3 dB; peak ~0.707
}

void TestMixerLpfHeavyDampingAt10kHz() {
    MixerRig rig;
    auto src = SineMono(kSampleRate, 10000.0f);
    rig.StartSineWithLpf(src.data(), static_cast<uint32_t>(src.size()),
                          /*lpfAmount*/ 0.7f);
    auto out = rig.Render(8);

    const size_t start = kBufferSize * 2;
    const float peak = Peak(out.data() + start, out.size() - start);
    std::printf("  10 kHz, lpfAmount=0.7 → peak=%.4f (expect <0.10, deep cut at ~1.7 kHz)\n", peak);
    EXPECT(peak < 0.10f);    // 18+ dB of attenuation, comfortably below 0.10
}

void TestMixerLpfPassesLowFreqAtHeavyDamping() {
    MixerRig rig;
    auto src = SineMono(kSampleRate, 200.0f);     // 200 Hz, well below 1.7 kHz cutoff
    rig.StartSineWithLpf(src.data(), static_cast<uint32_t>(src.size()),
                          /*lpfAmount*/ 0.7f);
    auto out = rig.Render(16);                     // wait out the filter ringup

    // Skip the first 2 buffers; biquad has transient at startup.
    const size_t start = 2 * kBufferSize * 2;
    const float peak = Peak(out.data() + start, out.size() - start);
    std::printf("  200 Hz, lpfAmount=0.7 → peak=%.3f (expect ≈ unattenuated ~0.7)\n", peak);
    EXPECT(peak > 0.6f);
}

// -------------------------------------------------------------------------
// End-to-end test; runtime with a custom geometry query that always
// reports occluded.
// -------------------------------------------------------------------------

class AlwaysOccludedQuery final : public IAudioGeometryQuery {
public:
    bool RaycastAudioOcclusion(const Vec3&, const Vec3&,
                                 AudioOcclusionHit& out) noexcept override {
        out.hit                 = true;
        out.distance            = 1.0f;
        out.materialAbsorption  = 1.0f;     // fully absorbent → max occlusion amount
        return true;
    }
};

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
    AudioBackendConfig cfg_{};
    IAudioRenderCallback* cb_ = nullptr;
    std::vector<float> scratch_;
};

float RunRuntimeAndMeasurePeak(bool occluded) {
    AudioConfig cfg;
    cfg.sampleRate                   = kSampleRate;
    cfg.bufferSize                   = kBufferSize;
    cfg.outputMode                   = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters     = 4;
    cfg.budget.maxRegisteredSounds   = 4;
    cfg.budget.maxStreamingAssets    = 1;
    cfg.budget.maxStreamingVoices    = 1;
    cfg.budget.maxVoiceSources       = 0;
    cfg.budget.maxOcclusionChecksPerFrame = 16;
    cfg.enableOcclusion              = true;

    AudioRuntime rt;
    auto backend = std::make_unique<OfflineBackend>();
    OfflineBackend* bp = backend.get();

    AudioRuntimeDependencies deps;
    deps.backend = std::move(backend);
    if (occluded) deps.geometryQuery = std::make_unique<AlwaysOccludedQuery>();

    if (rt.Initialize(cfg, std::move(deps)) != AudioResult::Success) {
        ++gFails;
        return 0.0f;
    }

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    rt.SetListener(lis);

    constexpr AudioSoundId kSnd = 1;
    auto pcm = SineMono(kSampleRate, 10000.0f);          // 10 kHz, 1 s
    rt.RegisterPcmSound(kSnd, pcm, kSampleRate, 1);

    SoundDefinition def;
    def.soundId           = kSnd;
    def.category          = AudioCategory::SFX;
    def.targetBus         = kBusMaster;
    def.spatialized       = true;
    def.looping           = true;
    def.occlusionEnabled  = true;
    def.attenuation.minDistance = 1.0f;
    def.attenuation.maxDistance = 100.0f;
    def.attenuation.volumeFloor = 0.5f;       // keep gain high so peak isn't dominated by attenuation
    rt.RegisterSoundDefinition(def);

    EmitterDescriptor ed;
    ed.soundId          = kSnd;
    ed.position         = {2.0f, 0.0f, 0.0f};            // 2 m away
    ed.targetBus        = kBusMaster;
    ed.category         = AudioCategory::SFX;
    ed.isLooping        = true;
    ed.isSpatialized    = true;
    ed.occlusionEnabled = true;
    ed.priority         = AudioPriority::Normal;
    ed.attenuation      = def.attenuation;
    auto h = rt.CreateEmitter(ed);
    if (!h) { ++gFails; return 0.0f; }

    // Tick a few times to let the spatializer stabilise (occlusion smoothing).
    std::vector<float> out;
    for (int i = 0; i < 20; ++i) {
        rt.Update(0.025f);
        bp->Render(kSampleRate / 40, out);                // 25 ms per tick
    }
    // Now measure peak over the next 250 ms of audio.
    std::vector<float> meas;
    for (int i = 0; i < 10; ++i) {
        rt.Update(0.025f);
        bp->Render(kSampleRate / 40, out);
        meas.insert(meas.end(), out.begin(), out.end());
    }

    rt.DestroyEmitter(h.value());
    rt.Shutdown();
    return Peak(meas.data(), meas.size());
}

void TestRuntimeOcclusionAttenuatesHighs() {
    const float clearPeak    = RunRuntimeAndMeasurePeak(/*occluded*/ false);
    const float occludedPeak = RunRuntimeAndMeasurePeak(/*occluded*/ true);
    std::printf("  10 kHz sine through runtime: clear peak=%.3f  occluded peak=%.3f\n",
                clearPeak, occludedPeak);
    EXPECT(clearPeak > 0.2f);                    // baseline shouldn't be dead
    EXPECT(occludedPeak < clearPeak * 0.5f);      // occlusion should cut HF energy by >2x
}

} // namespace

int main() {
    std::printf("[occlusion_lpf_test] running...\n");
    TestMixerLpfBypassedAtZero();
    TestMixerLpfHeavyDampingAt10kHz();
    TestMixerLpfPassesLowFreqAtHeavyDamping();
    TestRuntimeOcclusionAttenuatesHighs();
    if (gFails == 0) {
        std::printf("[occlusion_lpf_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[occlusion_lpf_test] %d failure(s)\n", gFails);
    return 1;
}
