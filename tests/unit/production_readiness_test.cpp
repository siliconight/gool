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

// tests/unit/production_readiness_test.cpp
//
// Targeted tests for the four shipping concerns we identified for an
// L4D2-style co-op shooter:
//
//   1. Material-aware occlusion: a concrete wall and a curtain produce
//      different gain + LPF responses through the same occlusion path.
//   2. One-shot timing race: a one-shot whose duration is shorter than
//      one Update tick survives long enough for the render thread to
//      see it, instead of being born-and-killed in the same tick.
//   3. Fade-out on Stop: posting Stop with a fadeOutMs ramps the voice
//      down monotonically across the fade window rather than cutting
//      it dead and producing a click.
//
// Item 4 (multi-tier sidechain ducking) is purely a composition of the
// existing bus + compressor primitives; coverage there is the
// `examples/multi_tier_ducking/` example and the README section, not a
// unit test.

#include "audio_engine/geometry_query.h"
#include "audio_engine/spatializer.h"
#include "audio_engine/spatial/default_spatializer.h"

#include "audio_engine/emitters/emitter_manager.h"
#include "audio_engine/mixer/audio_mixer.h"
#include "audio_engine/mixer/bus_graph.h"
#include "audio_engine/mixer/mixer_command.h"

#include <cmath>
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
constexpr float    kPi         = 3.14159265358979323846f;

// ============================================================
// Item 1: material differentiation through ResolveOcclusion +
// DefaultSpatializer.
// ============================================================

SpatialParams CalcWith(float occlusionAmount, float occlusionDamping) {
    SpatialEmitterView e;
    e.position         = {5.0f, 0.0f, 0.0f};
    e.spatialized      = true;
    e.minDistance      = 1.0f;
    e.maxDistance      = 100.0f;
    e.volumeFloor      = 1.0f;
    e.occlusionAmount  = occlusionAmount;
    e.occlusionDamping = occlusionDamping;

    SpatialListenerView l;
    l.position = {0.0f, 0.0f, 0.0f};
    l.forward  = {0.0f, 0.0f, -1.0f};
    l.up       = {0.0f, 1.0f, 0.0f};

    SpatialEnvironmentState env;

    DefaultSpatializer sp;
    return sp.Calculate(e, l, env);
}

void TestMaterialDifferentiation() {
    // Take three of the canonical material presets and check that the
    // engine's defaults turn into spatializer outputs that differ in
    // the right ways.
    float concreteAbs, concreteDamp;
    float curtainAbs,  curtainDamp;
    float glassAbs,    glassDamp;
    AudioMaterialDefaults(AudioMaterial::Concrete, concreteAbs, concreteDamp);
    AudioMaterialDefaults(AudioMaterial::Curtain,  curtainAbs,  curtainDamp);
    AudioMaterialDefaults(AudioMaterial::Glass,    glassAbs,    glassDamp);

    const auto concrete = CalcWith(concreteAbs, concreteDamp);
    const auto curtain  = CalcWith(curtainAbs,  curtainDamp);
    const auto glass    = CalcWith(glassAbs,    glassDamp);

    std::printf("  concrete: gain=%.3f  lpf=%.3f\n", concrete.gain, concrete.lowPassAmount);
    std::printf("  curtain : gain=%.3f  lpf=%.3f\n", curtain.gain,  curtain.lowPassAmount);
    std::printf("  glass   : gain=%.3f  lpf=%.3f\n", glass.gain,    glass.lowPassAmount);

    // Concrete is the "hard barrier" case: absorbs much (low gain) and
    // muffles much (high LPF amount).
    EXPECT(concrete.gain          < curtain.gain);
    EXPECT(concrete.gain          < glass.gain);
    EXPECT(concrete.lowPassAmount > glass.lowPassAmount);

    // Curtain is the "soft absorber": modest gain reduction, heavy HF
    // rolloff. Its LPF amount should be close to concrete's (both
    // damp highs aggressively) but its gain should be much higher
    // (sound passes through; only the highs die).
    EXPECT(curtain.gain          > concrete.gain);
    EXPECT(curtain.lowPassAmount > 0.5f);

    // Glass is the "see-through" case: low gain reduction, light HF
    // rolloff. Distinct from both.
    EXPECT(glass.gain          > 0.85f);
    EXPECT(glass.lowPassAmount < 0.10f);

    // The architectural assertion: the (gain, lpf) pairs are
    // materially different across all three. If a regression collapsed
    // damping back to absorption, curtain.lowPassAmount would equal
    // its gain-derived counterpart and we'd lose this signature.
    const float gainSpread = std::max({concrete.gain, curtain.gain, glass.gain})
                            - std::min({concrete.gain, curtain.gain, glass.gain});
    const float lpfSpread  = std::max({concrete.lowPassAmount, curtain.lowPassAmount, glass.lowPassAmount})
                            - std::min({concrete.lowPassAmount, curtain.lowPassAmount, glass.lowPassAmount});
    EXPECT(gainSpread > 0.4f);
    EXPECT(lpfSpread  > 0.4f);
}

// ============================================================
// Item 2: one-shot grace period.
// ============================================================

void TestOneShotGracePeriod() {
    EmitterManager mgr(/*maxActiveEmitters*/ 8);

    EmitterDescriptor d;
    d.soundId       = 1;
    d.position      = {0.0f, 0.0f, 0.0f};
    d.targetBus     = kBusMaster;
    d.category      = AudioCategory::SFX;
    d.priority      = AudioPriority::Normal;
    d.isLooping     = false;
    d.isSpatialized = false;

    auto h = mgr.Create(d, /*oneShot*/ true);
    EXPECT(static_cast<bool>(h));
    if (!h) return;

    // Manually set the one-shot lifetime to one frame's worth (sub-ms)
    // so without the grace flag, the very first tick would already
    // declare it expired.
    {
        auto* rec = mgr.Get(h.value());
        EXPECT(rec != nullptr);
        if (rec) {
            rec->oneShotFramesRemaining = 1.0;
            EXPECT(rec->firstTickPassed == false);
        }
    }

    int destroyCalls = 0;
    auto onDestroy = [&](EmitterHandle, EmitterRecord&) { ++destroyCalls; };

    // First tick. Tick interval much larger than the one-shot's
    // remaining frames. Without the grace flag, this kills the
    // emitter. With it, the flag flips to true, the decrement is
    // skipped, and the emitter survives.
    mgr.TickOneShots(/*framesPerSecond*/ 48000.0,
                       /*deltaSeconds*/    0.025,
                       onDestroy);
    EXPECT(destroyCalls == 0);
    EXPECT(mgr.IsValid(h.value()));
    {
        auto* rec = mgr.Get(h.value());
        EXPECT(rec && rec->firstTickPassed == true);
    }

    // Second tick. Now the decrement runs, frames fall to <= 0,
    // the emitter is destroyed, onDestroy fires.
    mgr.TickOneShots(48000.0, 0.025, onDestroy);
    EXPECT(destroyCalls == 1);
    EXPECT(!mgr.IsValid(h.value()));
}

// ============================================================
// Item 3: fade-out on Stop.
// ============================================================

struct MixerRig {
    BusGraph bg;
    AudioMixer mixer;
    std::vector<float> render;

    MixerRig()
        : mixer(/*maxMixVoices*/ 4,
                /*outputChannels*/ 2,
                /*commandRingDepth*/ 64,
                &bg,
                kSampleRate),
          render(static_cast<size_t>(kBufferSize) * 2)
    {
        BusGraphConfig cfg{};
        const auto rc = bg.Build(cfg, kSampleRate, /*channels*/ 2, kBufferSize);
        if (rc != AudioResult::Success) ++gFails;
    }

    void StartSine(const float* pcm, uint32_t frames, float gain = 0.5f) {
        MixerCommand cmd;
        cmd.kind          = MixerCommandKind::StartSound;
        cmd.mixSlot       = 1;
        cmd.gain          = gain;
        cmd.pan           = 0.0f;
        cmd.pitch         = 1.0f;
        cmd.lowPassAmount = 0.0f;
        cmd.targetBus     = kBusMaster;
        cmd.pcmData       = pcm;
        cmd.pcmFrames     = frames;
        cmd.pcmChannels   = 1;
        cmd.looping       = true;
        mixer.PostCommand(cmd);
    }

    void Stop(float fadeOutMs) {
        MixerCommand cmd;
        cmd.kind       = MixerCommandKind::Stop;
        cmd.mixSlot    = 1;
        cmd.fadeOutMs  = fadeOutMs;
        mixer.PostCommand(cmd);
    }

    std::vector<float> Render(uint32_t buffers) {
        std::vector<float> out;
        out.reserve(static_cast<size_t>(buffers) * kBufferSize * 2);
        for (uint32_t i = 0; i < buffers; ++i) {
            std::fill(render.begin(), render.end(), 0.0f);
            mixer.OnRender(render.data(), kBufferSize, 2);
            out.insert(out.end(), render.begin(), render.end());
        }
        return out;
    }
};

std::vector<float> SineMono(uint32_t frames, float hz) {
    std::vector<float> v(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        v[i] = std::sin(2.0f * kPi * hz * static_cast<float>(i) / kSampleRate);
    }
    return v;
}

// Peak amplitude across an interleaved-stereo block, both channels.
float PeakStereo(const float* d, size_t frames) {
    float p = 0.0f;
    for (size_t i = 0; i < frames; ++i) {
        const float l = std::fabs(d[i * 2 + 0]);
        const float r = std::fabs(d[i * 2 + 1]);
        if (l > p) p = l;
        if (r > p) p = r;
    }
    return p;
}

void TestFadeOutMonotonicAndSilent() {
    auto src = SineMono(kSampleRate, 440.0f);     // 1 s of 440 Hz
    constexpr float kFadeMs = 20.0f;
    constexpr uint32_t kFadeFrames = static_cast<uint32_t>((kFadeMs / 1000.0f) * kSampleRate);

    MixerRig rig;
    rig.StartSine(src.data(), static_cast<uint32_t>(src.size()), /*gain*/ 0.8f);

    // Warm up to steady state. 8 buffers ~= 43 ms of pre-Stop signal.
    auto warm = rig.Render(8);
    const float warmPeak = PeakStereo(warm.data() + warm.size() / 2, kBufferSize);
    std::printf("  pre-Stop peak (steady)        : %.3f\n", warmPeak);
    EXPECT(warmPeak > 0.5f);

    // Post Stop with the fade. Rendering enough buffers to span the
    // fade plus a tail. 6 buffers = 1536 frames = 32 ms covers the
    // 20 ms fade plus 12 ms of post-fade silence.
    rig.Stop(kFadeMs);
    auto post = rig.Render(6);

    // Window the fade region into ~2 ms slices and verify peaks in
    // each successive slice are non-increasing (allowing a tiny
    // tolerance for sine-zero-crossing artifacts inside a window).
    constexpr uint32_t kSliceFrames = 96;     // 2 ms
    const uint32_t numSlices = kFadeFrames / kSliceFrames;

    float prev = 1.0f;
    int regressionCount = 0;
    for (uint32_t s = 0; s < numSlices; ++s) {
        const size_t off = static_cast<size_t>(s) * kSliceFrames * 2;
        const float pk  = PeakStereo(post.data() + off, kSliceFrames);
        if (pk > prev + 0.02f) ++regressionCount;
        prev = pk;
    }
    std::printf("  fade-out monotonic regressions (out of %u slices): %d\n",
                numSlices, regressionCount);
    EXPECT(regressionCount == 0);

    // After the fade window, the voice should be silent. We measure
    // the last 5 ms of the rendered tail; any signal here means the
    // fade didn't actually shut the voice off.
    const size_t tailFrames = (kSampleRate * 5) / 1000;
    const size_t tailOffset = post.size() - tailFrames * 2;
    const float tailPeak = PeakStereo(post.data() + tailOffset, tailFrames);
    std::printf("  post-fade silence peak       : %.6f\n", tailPeak);
    EXPECT(tailPeak < 1e-4f);

    // Sanity: the fade-out path's first-slice peak should still be
    // close to the steady-state peak (the fade ramp begins at 1.0,
    // not somewhere lower).
    const float firstSlicePeak = PeakStereo(post.data(), kSliceFrames);
    std::printf("  fade first slice peak         : %.3f\n", firstSlicePeak);
    EXPECT(firstSlicePeak > 0.4f);
}

void TestImmediateStopWithoutFade() {
    // Sanity-check that Stop with fadeOutMs=0 (the legacy path) still
    // cuts immediately. We expect the very next render after Stop to
    // be silent, with no fade tail at all.
    auto src = SineMono(kSampleRate, 440.0f);
    MixerRig rig;
    rig.StartSine(src.data(), static_cast<uint32_t>(src.size()), 0.8f);
    rig.Render(8);

    rig.Stop(/*fadeOutMs*/ 0.0f);
    auto post = rig.Render(2);
    const float postPeak = PeakStereo(post.data(), post.size() / 2);
    std::printf("  immediate-stop peak (full render): %.6f\n", postPeak);
    EXPECT(postPeak < 1e-4f);
}

} // namespace

int main() {
    std::printf("[production_readiness_test] running...\n");

    std::printf(" -- Item 1: material differentiation --\n");
    TestMaterialDifferentiation();

    std::printf(" -- Item 2: one-shot grace period --\n");
    TestOneShotGracePeriod();

    std::printf(" -- Item 3: fade-out on Stop --\n");
    TestFadeOutMonotonicAndSilent();
    TestImmediateStopWithoutFade();

    if (gFails == 0) {
        std::printf("[production_readiness_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[production_readiness_test] %d failure(s)\n", gFails);
    return 1;
}
