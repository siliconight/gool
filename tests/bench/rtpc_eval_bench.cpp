// tests/bench/rtpc_eval_bench.cpp
//
// Measures the full RTPC evaluation hot path under realistic load:
// N emitters, M bindings each, K Update ticks. Reports cost per
// Update tick at various N×M combinations.
//
// What this exercises (per tick):
//   * EmitterManager::ForEach over N active emitters
//   * unordered_map<AudioSoundId, vector<SoundRtpcBinding>>::find for each
//   * unordered_map<AudioParameterId, float>::find per binding (param value)
//   * ApplyCurve + remap arithmetic
//   * ParameterSmoother::SetTarget per binding (the cost B1 measured)
//   * Then orchestrator step 9 reads back via Smoother::Get × 4 per emitter
//
// Decision rule: same as B1. If per-tick cost stays under ~100 µs at
// realistic N×M, no optimization is justified by data.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/backend.h"
#include "audio_engine/config.h"
#include "audio_engine/types.h"

#include "../bench/bench_util.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace audio;
using namespace audio_bench;

namespace {

class SilentBackend final : public IAudioBackend {
public:
    AudioResult Start(const AudioBackendConfig& cfg, IAudioRenderCallback*) override {
        cfg_ = cfg; return AudioResult::Success;
    }
    void     Stop() override {}
    uint32_t SampleRate() const noexcept override { return cfg_.sampleRate; }
    uint32_t BufferSize() const noexcept override { return cfg_.bufferSize; }
    uint32_t Channels()   const noexcept override { return cfg_.channels; }
    const char* Name()    const noexcept override { return "SilentBench"; }
private:
    AudioBackendConfig cfg_{};
};

// Build a runtime with N emitters playing N distinct sounds, each
// with M bindings. Returns the runtime ready for Update.
struct Scenario {
    AudioRuntime              runtime;
    std::vector<AudioSoundId> sounds;
    std::vector<AudioParameterId> params;

    static std::unique_ptr<Scenario> Build(uint32_t emitters, uint32_t bindingsPerSound,
                            uint32_t maxEmitterBudget = 1024) {
        auto sp = std::make_unique<Scenario>();
        Scenario& s = *sp;
        AudioConfig cfg;
        cfg.sampleRate = 48000;
        cfg.bufferSize = 256;
        cfg.outputMode = AudioOutputMode::Stereo;
        cfg.budget.maxActiveEmitters = maxEmitterBudget;
        cfg.maxGlobalParameters      = 1024;
        cfg.maxSoundRtpcBindings     = std::max(16u, emitters * bindingsPerSound + 16);

        AudioRuntimeDependencies deps;
        deps.backend = std::make_unique<SilentBackend>();
        auto rc = s.runtime.Initialize(cfg, std::move(deps));
        assert(rc == AudioResult::Success);

        // Listener so step 9 (UpdateParams pass) actually runs.
        AudioListener lis;
        lis.position = {0, 0, 0};
        lis.forward  = {0, 0, -1};
        lis.up       = {0, 1, 0};
        s.runtime.SetListener(lis);

        // Param pool. Uses a fixed 4 well-known parameters mapped to
        // the four RTPC targets. With M > 4 we cycle the param names.
        for (uint32_t i = 0; i < std::max(4u, bindingsPerSound); ++i) {
            std::string name = "param_" + std::to_string(i);
            s.params.push_back(HashParameterName(name));
            s.runtime.SetGlobalParameter(s.params.back(), 0.5f);
        }

        // Sounds + bindings + active emitters.
        std::vector<float> tinySamples(48, 0.5f);
        s.sounds.reserve(emitters);
        for (uint32_t i = 0; i < emitters; ++i) {
            const AudioSoundId sid = 0xB000 + i;
            s.sounds.push_back(sid);
            s.runtime.RegisterPcmSound(sid, tinySamples, 48000, 1);
            SoundDefinition def;
            def.soundId = sid;
            def.category = AudioCategory::SFX;
            def.targetBus = kBusMaster;
            def.spatialized = false;
            def.looping     = true;   // steady-state: emitters stay
                                        // active across the whole bench
                                        // run, so per-tick measurements
                                        // include real RTPC eval work
                                        // rather than spawn/retire churn.
            def.attenuation.minDistance = 1.0f;
            def.attenuation.maxDistance = 1000.0f;
            s.runtime.RegisterSoundDefinition(def);

            // Cycle bindings across the four targets. Skipped entirely
            // when bindingsPerSound==0 (M=0 baseline scenario).
            static constexpr RtpcTarget kTargets[4] = {
                RtpcTarget::Volume, RtpcTarget::Pitch,
                RtpcTarget::LowPassCutoff, RtpcTarget::ReverbSend
            };
            for (uint32_t b = 0; b < bindingsPerSound; ++b) {
                SoundRtpcBinding rb;
                rb.paramId       = s.params[b % s.params.size()];
                rb.target        = kTargets[b % 4];
                rb.curve         = RtpcCurve::Linear;
                rb.minValue      = 0.0f;
                rb.maxValue      = 1.0f;
                rb.minOutput     = 0.0f;
                rb.maxOutput     = 1.0f;
                rb.smoothingMs   = 50.0f;
                s.runtime.SetSoundRtpc(sid, rb);
            }

            s.runtime.SubmitEvent(
                AudioEvent::MakePlaySoundAtLocation(sid, Vec3{0, 0, 0}));
        }

        for (int i = 0; i < 3; ++i) s.runtime.Update(1.0f / 60.0f);
        return sp;
    }
};

} // namespace

int main() {
    BenchSuite suite{"rtpc_eval_path"};

    struct Combo { uint32_t emitters; uint32_t bindings; };
    static constexpr Combo combos[] = {
        // M=0 baseline: emitters playing, no RTPC bindings, no
        // SetTarget calls in Update — isolates step-9 + spatializer
        // + mixer cost. Difference vs M=1, M=4 reveals what RTPC
        // evaluation actually adds.
        { 64,  0},
        {128,  0},
        {256,  0},
        { 16,  1},
        { 64,  1},
        { 64,  4},
        {128,  1},
        {128,  4},
        {256,  1},
        {256,  4},
    };

    for (auto c : combos) {
        char label[80];
        std::snprintf(label, sizeof(label),
                       "Update tick (full path)  N=%u  M=%u",
                       c.emitters, c.bindings);

        auto sc = Scenario::Build(c.emitters, c.bindings,
                                    /*budget*/ std::max(c.emitters, 1024u));

        // Steady-state: looping sounds keep emitters active for the
        // entire run, so each Update tick measures real per-tick
        // RTPC evaluation cost rather than spawn/retire overhead.
        suite.Run(label, 1000, [&] {
            sc->runtime.Update(1.0f / 60.0f);
        });

        sc->runtime.Shutdown();
    }

    return suite.Summary();
}
