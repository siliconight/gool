// tests/bench/audio_mixer_bench.cpp
//
// Microbenchmark for AudioMixer's render-thread hot path. Two functions
// dominate that path and are also the two on the lizard threshold list:
//
//   * DrainCommands       — drains the SPSC ring at the top of every
//                            OnRender. Cost scales with ring depth.
//   * MixVoiceSound_      — per-voice Sound-mode body. Cost scales with
//                            (frames * channels * activeVoices) and
//                            branches into LPF / binaural / loop-xfade
//                            depending on per-voice flags.
//
// Both are private; the bench drives them via the public OnRender() and
// PostCommand() surfaces, which is how the production render thread
// reaches them anyway. To isolate DrainCommands from the mix path the
// bench varies command pressure independently of active-voice count.
//
// What this bench is for:
//   * Establish a v0.20.1 baseline for the mixer hot path. The existing
//     baseline in docs/perf.md (the ParameterSmoother + RTPC eval
//     numbers) is from v0.7.2 and does not measure the mixer itself.
//   * Give v0.21's planned decomposition of DrainCommands and
//     MixVoiceSound_ a "before" to compare against.
//   * Make voice-count scaling visible — if MixVoiceSound_ is linear
//     in N (it should be; one pass per voice), the ns_per_op column at
//     fixed frames will scale linearly with the active-voices column.
//
// Scenarios:
//   A. OnRender at N active Sound voices, mono source, equal-power pan,
//      no LPF, no fade, no binaural. The hottest baseline. N varies.
//   B. OnRender at N active Sound voices with LPF engaged on every voice
//      (lowPassAmount = 0.5). Measures the cost of the per-frame biquad.
//   C. OnRender at N active Sound voices in binaural mode. Measures the
//      cost of the dual delay lines + per-ear LPF.
//   D. DrainCommands at K commands posted per render, N=64 active voices
//      in mono+pan mode. K varies; the per-command cost is the delta
//      against scenario A at the same N.
//
// All scenarios use 256-frame stereo output buffers (the common
// production buffer size at 48 kHz / ~5.3 ms).

#include "audio_engine/mixer/audio_mixer.h"
#include "audio_engine/mixer/bus_graph.h"
#include "audio_engine/mixer/mixer_command.h"
#include "audio_engine/bus.h"
#include "audio_engine/result.h"

#include "../bench/bench_util.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace audio;
using namespace audio_bench;

namespace {

// One small PCM buffer shared by every voice in the bench. The mixer
// does not own pcmData; production keeps the underlying asset pinned
// for the mixer's lifetime, which here is the bench process lifetime.
// 480 frames mono = exactly 10 ms at 48 kHz, looping; long enough to
// span typical render buffers without wrapping every callback,
// short enough to fit comfortably in L1.
constexpr uint32_t kSrcFrames   = 480;
constexpr uint32_t kSrcChannels = 1;
constexpr uint32_t kRenderFrames    = 256;
constexpr uint32_t kRenderChannels  = 2;
constexpr uint32_t kSampleRate      = 48000;

std::vector<float> MakeSinePcm() {
    std::vector<float> samples(kSrcFrames * kSrcChannels);
    constexpr float kTwoPi = 6.2831853f;
    for (uint32_t i = 0; i < kSrcFrames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
        samples[i] = 0.25f * std::sin(kTwoPi * 440.0f * t);
    }
    return samples;
}

// Wraps an AudioMixer + BusGraph configured with a master-only graph,
// ready to drive via PostCommand + OnRender.
struct MixerHarness {
    BusGraph                 graph;
    std::unique_ptr<AudioMixer> mixer;
    std::vector<float>       output;

    explicit MixerHarness(uint32_t maxVoices) : output(kRenderFrames * kRenderChannels) {
        BusGraphConfig bcfg;
        bcfg.busCount = 0;     // auto-creates a master-only graph
        const auto rc = graph.Build(bcfg, kSampleRate, kRenderChannels, kRenderFrames);
        assert(rc == AudioResult::Success);

        mixer = std::make_unique<AudioMixer>(
            maxVoices,
            kRenderChannels,
            /*commandRingDepth*/ 4096,
            &graph,
            kSampleRate);
    }
};

// Build a StartSound command for a Sound-mode voice.
MixerCommand MakeStartSound(uint32_t slot,
                            const float* pcm,
                            uint32_t frames,
                            uint32_t channels,
                            float lpfAmount = 0.0f,
                            bool useBinaural = false) {
    MixerCommand c{};
    c.kind         = MixerCommandKind::StartSound;
    c.mixSlot      = slot;
    c.gain         = 1.0f;
    c.pan          = 0.0f;       // centered; equal-power pan path
    c.pitch        = 1.0f;
    c.lowPassAmount = lpfAmount;
    c.pcmData      = pcm;
    c.pcmFrames    = frames;
    c.pcmChannels  = channels;
    c.looping      = true;       // steady-state: voices never end during the bench
    c.targetBus    = kBusMaster;
    c.useBinaural  = useBinaural;
    if (useBinaural) {
        c.gainL          = 0.7071f;
        c.gainR          = 0.7071f;
        c.delaySamplesL  = 4.0f;     // representative ITD
        c.delaySamplesR  = 0.0f;
        c.lpfAmountL     = 0.2f;
        c.lpfAmountR     = 0.4f;
    }
    return c;
}

// Spin up N voices in the mixer by posting N StartSound commands and
// running one OnRender to drain them. After this returns, the next
// OnRender call measures pure steady-state mix cost (the command ring
// is empty).
void PrimeVoices(MixerHarness& h,
                 const std::vector<float>& pcm,
                 uint32_t activeVoices,
                 float lpfAmount = 0.0f,
                 bool useBinaural = false) {
    for (uint32_t i = 0; i < activeVoices; ++i) {
        const bool posted = h.mixer->PostCommand(MakeStartSound(
            i, pcm.data(), kSrcFrames, kSrcChannels, lpfAmount, useBinaural));
        assert(posted);
    }
    // Drain the start commands and prime cursor state.
    h.mixer->OnRender(h.output.data(), kRenderFrames, kRenderChannels);
}

// ---------------------------------------------------------------------------
// Scenario A: steady-state OnRender at N active Sound voices, no LPF,
// no binaural. The hottest path the production render thread runs.
// ---------------------------------------------------------------------------
void RunScenarioA(BenchSuite& suite, const std::vector<float>& pcm) {
    for (uint32_t n : {1u, 8u, 32u, 64u, 128u, 256u}) {
        MixerHarness h(/*maxVoices*/ std::max(n, 16u));
        PrimeVoices(h, pcm, n);

        char label[80];
        std::snprintf(label, sizeof(label),
                       "A. OnRender Sound mono+pan   N=%u", n);

        suite.Run(label, 5000, [&] {
            h.mixer->OnRender(h.output.data(), kRenderFrames, kRenderChannels);
            DoNotOptimize(h.output[0]);
        });
    }
}

// ---------------------------------------------------------------------------
// Scenario B: same as A but with a non-trivial lowPassAmount on every
// voice. Exercises the biquad LPF inside MixVoiceSound_'s per-frame
// loop (the `lpfAmount > 0.001f` branch).
// ---------------------------------------------------------------------------
void RunScenarioB(BenchSuite& suite, const std::vector<float>& pcm) {
    for (uint32_t n : {32u, 64u, 128u, 256u}) {
        MixerHarness h(std::max(n, 16u));
        PrimeVoices(h, pcm, n, /*lpfAmount*/ 0.5f);

        char label[80];
        std::snprintf(label, sizeof(label),
                       "B. OnRender Sound + LPF      N=%u", n);

        suite.Run(label, 5000, [&] {
            h.mixer->OnRender(h.output.data(), kRenderFrames, kRenderChannels);
            DoNotOptimize(h.output[0]);
        });
    }
}

// ---------------------------------------------------------------------------
// Scenario C: binaural mode. Each voice runs dual delay lines (ITD) and
// per-ear LPFs (ILD). The most expensive sub-path inside MixVoiceSound_.
// ---------------------------------------------------------------------------
void RunScenarioC(BenchSuite& suite, const std::vector<float>& pcm) {
    for (uint32_t n : {32u, 64u, 128u, 256u}) {
        MixerHarness h(std::max(n, 16u));
        PrimeVoices(h, pcm, n, /*lpfAmount*/ 0.0f, /*useBinaural*/ true);

        char label[80];
        std::snprintf(label, sizeof(label),
                       "C. OnRender Sound binaural   N=%u", n);

        suite.Run(label, 5000, [&] {
            h.mixer->OnRender(h.output.data(), kRenderFrames, kRenderChannels);
            DoNotOptimize(h.output[0]);
        });
    }
}

// ---------------------------------------------------------------------------
// Scenario D: command-drain pressure. Holds active voices at N=64 and
// varies the number of commands posted per OnRender. UpdateParams is the
// production-realistic churn pattern (spatializer pushes new pan/gain/
// pitch every tick).
//
// Delta against scenario A at N=64 is per-command drain cost.
// ---------------------------------------------------------------------------
void RunScenarioD(BenchSuite& suite, const std::vector<float>& pcm) {
    constexpr uint32_t kN = 64;
    for (uint32_t commandsPerRender : {0u, 16u, 64u, 128u, 256u}) {
        MixerHarness h(/*maxVoices*/ 256);
        PrimeVoices(h, pcm, kN);

        char label[80];
        std::snprintf(label, sizeof(label),
                       "D. OnRender + %u cmds/render N=%u", commandsPerRender, kN);

        suite.Run(label, 5000, [&] {
            // Post K UpdateParams commands targeting the active slots,
            // cycling through them. PostCommand can return false if the
            // ring fills — at 4096 depth and steady drainage that
            // doesn't happen for the K values used here.
            for (uint32_t i = 0; i < commandsPerRender; ++i) {
                MixerCommand c{};
                c.kind    = MixerCommandKind::UpdateParams;
                c.mixSlot = i % kN;
                c.gain    = 0.5f;
                c.pan     = 0.1f;
                c.pitch   = 1.0f;
                c.targetBus = kBusMaster;
                (void)h.mixer->PostCommand(c);
            }
            h.mixer->OnRender(h.output.data(), kRenderFrames, kRenderChannels);
            DoNotOptimize(h.output[0]);
        });
    }
}

} // namespace

int main() {
    const auto pcm = MakeSinePcm();

    BenchSuite suite{"audio_mixer"};
    RunScenarioA(suite, pcm);
    RunScenarioB(suite, pcm);
    RunScenarioC(suite, pcm);
    RunScenarioD(suite, pcm);
    return suite.Summary();
}
