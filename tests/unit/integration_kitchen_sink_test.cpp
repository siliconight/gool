// tests/unit/integration_kitchen_sink_test.cpp
//
// "Kitchen-sink" integration test: a 5-second simulated session that
// exercises every subsystem at once and asserts on cross-cutting
// invariants. The unit tests in this directory each isolate one
// feature; this one verifies the features compose without stepping on
// each other under sustained load.
//
// Scenario:
//   * 8-slot emitter pool, deliberately tight to force priority eviction
//   * Master + Reverb-bus graph, Freeverb on the reverb bus
//   * Listener that translates over time (drives Doppler + spatial pan)
//   * Streaming music asset on a dedicated streaming voice
//   * Periodic gunshot one-shots at Critical priority
//   * Higher-rate ambient one-shots at Low priority (forces eviction
//     contention against the gunshots)
//   * A high-velocity persistent emitter doing a fly-by (Doppler check)
//   * A custom IAudioGeometryQuery that occludes everything > 30 m away
//
// At the end, we destroy all dry sources and render some silence to
// measure the residual reverb tail; confirming the wet path stayed
// alive across the chaotic dry mix.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/bus.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/events.h"
#include "audio_engine/geometry_query.h"
#include "audio_engine/types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

constexpr uint32_t kSampleRate    = 48000;
constexpr uint32_t kBufferSize    = 256;
constexpr uint32_t kTickMs        = 25;
constexpr uint32_t kFramesPerTick = (kSampleRate * kTickMs) / 1000;
constexpr float    kPi            = 3.14159265358979323846f;

constexpr AudioSoundId kSndMusic       = 1;     // streaming
constexpr AudioSoundId kSndGunshot     = 2;     // Critical priority
constexpr AudioSoundId kSndAmbientBug  = 3;     // Low priority
constexpr AudioSoundId kSndBulletWhip  = 4;     // moving emitter

float Peak(const float* d, size_t n) {
    float p = 0.0f;
    for (size_t i = 0; i < n; ++i) { const float a = std::fabs(d[i]); if (a > p) p = a; }
    return p;
}
float Rms(const float* d, size_t n) {
    if (n == 0) return 0.0f;
    double a = 0.0;
    for (size_t i = 0; i < n; ++i) a += static_cast<double>(d[i]) * d[i];
    return static_cast<float>(std::sqrt(a / static_cast<double>(n)));
}

std::vector<float> SineMono(uint32_t frames, float hz, float amp = 0.5f) {
    std::vector<float> v(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        v[i] = amp * std::sin(2.0f * kPi * hz * static_cast<float>(i)
                                       / static_cast<float>(kSampleRate));
    }
    return v;
}

std::vector<float> NoiseBurst(uint32_t frames, float amp = 0.6f) {
    std::vector<float> v(frames);
    uint32_t s = 1664525u;
    for (uint32_t i = 0; i < frames; ++i) {
        s = s * 1103515245u + 12345u;
        const float n = (static_cast<float>(s & 0xFFFF) / 32768.0f) - 1.0f;
        const float env = 1.0f - static_cast<float>(i) / static_cast<float>(frames);
        v[i] = amp * env * n;
    }
    return v;
}

class FarFieldOccluder final : public IAudioGeometryQuery {
public:
    explicit FarFieldOccluder(float thresholdDistance) : thr_(thresholdDistance) {}
    bool RaycastAudioOcclusion(const Vec3& from, const Vec3& to,
                                AudioOcclusionHit& out) noexcept override {
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float dz = to.z - from.z;
        const float d  = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (d > thr_) {
            out.hit                = true;
            out.distance           = d * 0.5f;
            out.materialAbsorption = 0.85f;
            return true;
        }
        out.hit = false;
        return false;
    }
private:
    float thr_;
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

void RunIntegration() {
    AudioConfig cfg;
    cfg.sampleRate                 = kSampleRate;
    cfg.bufferSize                 = kBufferSize;
    cfg.outputMode                 = AudioOutputMode::Stereo;
    cfg.budget.maxActiveEmitters   = 8;     // tight: forces eviction stress
    cfg.budget.maxRegisteredSounds = 16;
    cfg.budget.maxStreamingAssets  = 2;
    cfg.budget.maxStreamingVoices  = 1;
    cfg.budget.maxVoiceSources     = 0;
    cfg.budget.maxOcclusionChecksPerFrame = 16;
    cfg.enableOcclusion            = true;
    cfg.enableAirAbsorption        = true;
    cfg.enableDoppler              = true;
    cfg.globalReverbSend           = 0.45f;
    cfg.airAbsorptionPerMeter      = 1.0f / 200.0f;

    cfg.busGraph.busCount = 2;
    cfg.busGraph.buses[0].id     = kBusMaster;
    cfg.busGraph.buses[1].id     = kBusReverb;
    cfg.busGraph.buses[1].parent = kBusMaster;
    cfg.busGraph.buses[1].outputGainDb = -3.0f;
    cfg.busGraph.buses[1].effects[0].kind            = EffectKind::Reverb;
    cfg.busGraph.buses[1].effects[0].reverbDecay     = 0.75f;
    cfg.busGraph.buses[1].effects[0].reverbHfDamping = 0.4f;
    cfg.busGraph.buses[1].effects[0].reverbDryGainDb = -100.0f;  // send/return: wet only
    cfg.busGraph.buses[1].effects[0].reverbWetGainDb = 0.0f;
    cfg.busGraph.buses[1].effectCount                = 1;

    AudioRuntime rt;
    auto backend = std::make_unique<OfflineBackend>();
    OfflineBackend* bp = backend.get();

    AudioRuntimeDependencies deps;
    deps.backend        = std::move(backend);
    deps.geometryQuery  = std::make_unique<FarFieldOccluder>(/*thresholdDistance*/ 30.0f);

    if (rt.Initialize(cfg, std::move(deps)) != AudioResult::Success) {
        std::fprintf(stderr, "Initialize failed\n");
        ++gFails; return;
    }

    AudioListener lis;
    lis.position = {0.0f, 0.0f, 0.0f};
    rt.SetListener(lis);

    {
        std::vector<float> music = SineMono(kSampleRate * 4, 220.0f, 0.4f);
        const auto rc = rt.RegisterStreamingSoundFromMemory(
            kSndMusic, std::move(music), kSampleRate, /*channels*/ 1);
        EXPECT(rc == AudioResult::Success);
        SoundDefinition def;
        def.soundId        = kSndMusic;
        def.category       = AudioCategory::Music;
        def.targetBus      = kBusMaster;
        def.spatialized    = false;
        def.looping        = true;
        rt.RegisterSoundDefinition(def);
    }
    {
        rt.RegisterPcmSound(kSndGunshot, NoiseBurst(kSampleRate / 5), kSampleRate, 1);
        SoundDefinition def;
        def.soundId        = kSndGunshot;
        def.category       = AudioCategory::SFX;
        def.targetBus      = kBusMaster;
        def.spatialized    = true;
        def.looping        = false;
        def.occlusionEnabled        = true;
        def.attenuation.minDistance = 1.0f;
        def.attenuation.maxDistance = 80.0f;
        def.attenuation.volumeFloor = 0.0f;
        rt.RegisterSoundDefinition(def);
    }
    {
        rt.RegisterPcmSound(kSndAmbientBug, SineMono(kSampleRate * 20 / 100, 1200.0f, 0.3f),
                            kSampleRate, 1);
        SoundDefinition def;
        def.soundId        = kSndAmbientBug;
        def.category       = AudioCategory::Ambience;
        def.targetBus      = kBusMaster;
        def.spatialized    = true;
        def.looping        = false;
        def.occlusionEnabled        = true;
        def.attenuation.minDistance = 1.0f;
        def.attenuation.maxDistance = 50.0f;
        def.attenuation.volumeFloor = 0.0f;
        rt.RegisterSoundDefinition(def);
    }
    {
        rt.RegisterPcmSound(kSndBulletWhip, NoiseBurst(kSampleRate / 10, 0.5f),
                            kSampleRate, 1);
        SoundDefinition def;
        def.soundId        = kSndBulletWhip;
        def.category       = AudioCategory::SFX;
        def.targetBus      = kBusMaster;
        def.spatialized    = true;
        def.looping        = true;
        def.occlusionEnabled        = false;
        def.attenuation.minDistance = 1.0f;
        def.attenuation.maxDistance = 100.0f;
        def.attenuation.volumeFloor = 0.0f;
        rt.RegisterSoundDefinition(def);
    }

    EmitterDescriptor musicEd;
    musicEd.soundId          = kSndMusic;
    musicEd.position         = {0.0f, 0.0f, 0.0f};
    musicEd.targetBus        = kBusMaster;
    musicEd.category         = AudioCategory::Music;
    musicEd.isLooping        = true;
    musicEd.isSpatialized    = false;
    musicEd.priority         = AudioPriority::High;
    auto musicH = rt.CreateEmitter(musicEd);
    EXPECT(static_cast<bool>(musicH));

    EmitterDescriptor whipEd;
    whipEd.soundId           = kSndBulletWhip;
    whipEd.position          = {-50.0f, 0.0f, 0.0f};
    whipEd.velocity          = { 30.0f, 0.0f, 0.0f};
    whipEd.targetBus         = kBusMaster;
    whipEd.category          = AudioCategory::SFX;
    whipEd.isLooping         = true;
    whipEd.isSpatialized     = true;
    whipEd.priority          = AudioPriority::High;
    whipEd.attenuation.minDistance = 1.0f;
    whipEd.attenuation.maxDistance = 100.0f;
    whipEd.attenuation.volumeFloor = 0.0f;
    auto whipH = rt.CreateEmitter(whipEd);
    EXPECT(static_cast<bool>(whipH));

    constexpr uint32_t kSimTicks = 200;     // 5 seconds at 25 ms/tick
    std::vector<float> dryCapture;
    dryCapture.reserve(kSimTicks * kFramesPerTick * 2);

    uint32_t critFires = 0;
    uint32_t ambFires  = 0;

    for (uint32_t i = 0; i < kSimTicks; ++i) {
        const float t = static_cast<float>(i) * (kTickMs / 1000.0f);

        lis.position = { std::sin(t * 0.5f) * 5.0f,
                         0.0f,
                         std::cos(t * 0.5f) * 5.0f };
        rt.SetListener(lis);

        if (whipH) {
            const Vec3 pos { -50.0f + 30.0f * t, 0.0f, 0.0f };
            const Vec3 fwd {  1.0f, 0.0f, 0.0f };
            const Vec3 vel {  30.0f, 0.0f, 0.0f };
            rt.SetEmitterTransform(whipH.value(), pos, fwd, vel);
        }

        if (i % 10 == 0) {
            AudioEvent ev;
            ev.type     = AudioEventType::PlaySoundAtLocation;
            ev.soundId  = kSndGunshot;
            ev.position = { std::sin(t * 1.3f) * 30.0f,
                            0.0f,
                            std::cos(t * 0.7f) * 30.0f };
            ev.priority = AudioPriority::Critical;
            rt.SubmitEvent(ev);
            ++critFires;
        }
        // Ambient bugs every tick; at 200 ms duration with an 8-slot
        // pool minus 2 persistent emitters minus ~1 active gunshot, we
        // have ~5 free slots. With ambients firing every 25 ms and lasting
        // 200 ms, ~8 are alive at peak; guaranteed pool saturation,
        // forcing both eviction (when a Critical arrives) and dropping
        // (when a Low arrives but the pool is full of Highs/Criticals).
        if (true) {
            AudioEvent ev;
            ev.type     = AudioEventType::PlaySoundAtLocation;
            ev.soundId  = kSndAmbientBug;
            const float r = 20.0f + 30.0f * std::sin(t * 2.1f + 1.0f);
            ev.position = { r * std::sin(t * 1.7f),
                            0.0f,
                            r * std::cos(t * 1.1f) };
            ev.priority = AudioPriority::Low;
            rt.SubmitEvent(ev);
            ++ambFires;
        }

        rt.Update(kTickMs / 1000.0f);
        std::vector<float> out;
        bp->Render(kFramesPerTick, out);
        dryCapture.insert(dryCapture.end(), out.begin(), out.end());
    }

    const auto stats = rt.GetStats();
    std::printf("\n  --- Stats after 5-second simulation ---\n");
    std::printf("  total render callbacks: %llu\n", (unsigned long long)stats.totalRenderCallbacks);
    std::printf("  active emitters (snapshot): %u\n", stats.activeEmitters);
    std::printf("  mixer voices active (snapshot): %u\n", stats.mixerVoicesActive);
    std::printf("  render underruns: %llu\n", (unsigned long long)stats.renderUnderruns);
    std::printf("  one-shot evictions: %llu\n", (unsigned long long)stats.oneShotEvictions);
    std::printf("  one-shots dropped (full pool): %llu\n",
                (unsigned long long)stats.oneShotsDroppedFullPool);
    std::printf("  events fired: %u Critical / %u Low\n", critFires, ambFires);

    const float capPeak = Peak(dryCapture.data(), dryCapture.size());
    const float capRms  = Rms(dryCapture.data(), dryCapture.size());
    std::printf("  output peak=%.3f  rms=%.4f\n", capPeak, capRms);

    EXPECT(stats.renderUnderruns == 0);
    EXPECT(stats.totalRenderCallbacks > 800);
    EXPECT(stats.oneShotEvictions > 0);
    EXPECT(stats.oneShotsDroppedFullPool > 0);
    EXPECT(capRms  > 0.005f);
    EXPECT(capPeak > 0.05f);

    if (whipH)  rt.DestroyEmitter(whipH.value());
    if (musicH) rt.DestroyEmitter(musicH.value());

    std::vector<float> tail;
    for (uint32_t i = 0; i < 10; ++i) {
        rt.Update(kTickMs / 1000.0f);
        std::vector<float> out;
        bp->Render(kFramesPerTick, out);
        tail.insert(tail.end(), out.begin(), out.end());
    }
    const float tailRms = Rms(tail.data(), tail.size());
    std::printf("  reverb tail rms (after sources stopped, 250 ms window): %.5f\n", tailRms);

    EXPECT(tailRms > 1e-4f);

    rt.Shutdown();
}

} // namespace

int main() {
    std::printf("[integration_kitchen_sink_test] running...\n");
    RunIntegration();
    if (gFails == 0) {
        std::printf("[integration_kitchen_sink_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[integration_kitchen_sink_test] %d failure(s)\n", gFails);
    return 1;
}
