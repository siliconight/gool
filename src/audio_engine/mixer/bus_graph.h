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

// audio_engine/mixer/bus_graph.h
//
// Validates the host's bus configuration, builds the topological render
// order (so any sidechain-referenced bus is processed before the bus that
// references it), allocates per-bus input/output buffers, and instantiates
// the DSP effect chain for each bus.
//
// Lifetime: constructed at AudioRuntime::Initialize() with the
// BusGraphConfig from AudioConfig. All buffers and effect instances are
// allocated here; the render thread does not allocate.
//
// Render-thread access pattern:
//   1. Mixer clears every bus's input buffer.
//   2. Mixer accumulates active voices into their target bus's input buffer.
//   3. For each bus in `RenderOrder()`:
//        a. copy input -> output
//        b. run effect chain on output (sidechain pointer resolved by graph)
//        c. if not silent, sum bus.output * bus.outputGain into bus.parent.input
//   4. master.output is the final mix.

#ifndef AUDIO_ENGINE_MIXER_BUS_GRAPH_H
#define AUDIO_ENGINE_MIXER_BUS_GRAPH_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio_engine/bus.h"
#include "audio_engine/result.h"
#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/util/aligned_float_buffer.h"

namespace audio {

class BusGraph {
public:
    BusGraph() = default;

    // Validates `cfg`, builds the graph, and Prepare()s every effect.
    // `frames` is the maximum number of frames the render callback will
    // request (used to size per-bus buffers).
    AudioResult Build(const BusGraphConfig& cfg,
                       uint32_t              sampleRate,
                       uint32_t              channels,
                       uint32_t              maxFrames);

    // Number of buses defined in the graph (always >= 1; master always exists).
    uint32_t BusCount() const noexcept { return static_cast<uint32_t>(buses_.size()); }

    // Resolves a BusId to its index in the buses_ array, or kInvalidIndex.
    static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
    uint32_t IndexOf(BusId id) const noexcept;

    // The render order; bus indices. Iterate this in the render path.
    const std::vector<uint32_t>& RenderOrder() const noexcept { return renderOrder_; }

    // The category-to-bus map (resolved to indices, kInvalidIndex when unmapped).
    uint32_t IndexForCategory(AudioCategory cat) const noexcept;

    // ---- Render-thread accessors -----------------------------------------
    // The mixer accumulates voices into a bus's *input* buffer, then walks
    // RenderOrder to process each bus into its *output* buffer. Both are
    // contiguous interleaved float[frames * channels].

    float*       InputBuffer(uint32_t busIndex)        noexcept { return buses_[busIndex]->input.data(); }
    float*       OutputBuffer(uint32_t busIndex)       noexcept { return buses_[busIndex]->output.data(); }
    const float* OutputBufferConst(uint32_t busIndex)  const noexcept { return buses_[busIndex]->output.data(); }
    uint32_t     ParentIndex(uint32_t busIndex)        const noexcept { return buses_[busIndex]->parentIndex; }
    bool         IsSilent(uint32_t busIndex)           const noexcept { return buses_[busIndex]->silent; }
    uint32_t     MasterIndex()                         const noexcept { return masterIndex_; }

    // Clears every bus's input buffer.
    void ClearAllInputBuffers(uint32_t frames, uint32_t channels) noexcept;

    // Returns the per-bus, per-callback gain (already smoothed). For now
    // gain is applied during the parent-sum step using the same value for
    // the whole callback (no per-sample ramp at the bus level; effects
    // handle internal smoothing).
    float OutputGainLinear(uint32_t busIndex) const noexcept {
        return buses_[busIndex]->outputGainLinear.load(std::memory_order_relaxed);
    }

    // ---- Effect access ---------------------------------------------------

    // Effects in the chain for a bus, in declared order.
    IDspEffect* EffectAt(uint32_t busIndex, uint32_t effectIndex) noexcept;
    uint32_t    EffectCount(uint32_t busIndex) const noexcept;

    // For each effect, the bus index that supplies its sidechain (or
    // kInvalidIndex when the effect has no sidechain).
    uint32_t SidechainSourceIndex(uint32_t busIndex, uint32_t effectIndex) const noexcept;

    // v0.28.0: effect-chain introspection for the mixer dock's
    // upcoming effect-edit UI (Phase 3.3c-2). Both are thin
    // forwarders to the underlying IDspEffect implementation.
    // Out-of-range indices return EffectKind::None and 0.0f
    // respectively — symmetric with the SetEffectParameter
    // "ignored if invalid" pattern.
    EffectKind EffectKindAt(uint32_t busIndex,
                             uint32_t effectIndex) const noexcept;
    float      EffectParameterAt(uint32_t busIndex,
                                  uint32_t effectIndex,
                                  uint16_t paramId) const noexcept;

    // ---- Control-thread parameter writes ---------------------------------

    AudioResult SetBusOutputGainDb(BusId id, float gainDb) noexcept;

    // ---- Diagnostics ------------------------------------------------------

    uint32_t Channels()    const noexcept { return channels_; }
    uint32_t MaxFrames()   const noexcept { return maxFrames_; }
    uint32_t SampleRate()  const noexcept { return sampleRate_; }

    // ---- v0.24.0: per-bus metering ---------------------------------------
    //
    // Render-thread side (called from audio_mixer.cpp::RunBusGraph after
    // each bus's effect chain has run). Scans `output` for max abs and
    // CAS-updates the bus's atomic peak. Cheap (one pass, no allocations).
    void CapturePeakLinear(uint32_t busIndex,
                            const float* output,
                            uint32_t     frames,
                            uint32_t     channels) noexcept;

    // Control-thread side. Reads the current peak and atomically resets
    // it to 0.0f so the next read covers samples since this call.
    // Returns the linear (not dB) peak abs sample observed.
    float ReadAndResetBusPeakLinear(uint32_t busIndex) noexcept;

    // Read-only access to a bus's debug name (copied from BusConfig at
    // Build time). Empty string if busIndex out of range. The pointer
    // is stable for the lifetime of the BusGraph; never null.
    const char* BusName(uint32_t busIndex) const noexcept;

    // ---- v0.27.0: per-bus mute / solo / effect-bypass ---------------------
    //
    // Setters write the atomic bools directly (cheap, no command queue
    // overhead — these are user toggles, not per-sample parameters that
    // need smoothing). Getters read with relaxed memory ordering. The
    // render-thread side of these is inside AudioMixer::RunBusGraph
    // which reads them once per bus per callback and applies the
    // gating logic.
    //
    // Out-of-range busIndex: setters are no-ops, getters return false.

    void SetBusMuted(uint32_t busIndex, bool muted) noexcept;
    void SetBusSoloed(uint32_t busIndex, bool soloed) noexcept;
    void SetBusEffectsBypassed(uint32_t busIndex, bool bypassed) noexcept;

    bool IsBusMuted(uint32_t busIndex) const noexcept;
    bool IsBusSoloed(uint32_t busIndex) const noexcept;
    bool IsBusEffectsBypassed(uint32_t busIndex) const noexcept;

    // Helper used by the mixer at the top of each render to decide
    // whether to apply "solo mode" gating. O(N) over kMaxBuses; one
    // atomic load per bus. Called once per audio callback.
    bool AnyBusSoloed() const noexcept;

    // v0.27.1: compute a bitmask of buses that are EITHER soloed OR
    // ancestors of soloed buses (i.e., on the audible path from a
    // soloed bus to the device output). Used by the mixer to decide
    // which buses should be silenced in solo mode.
    //
    // Why: when a child bus is soloed, the master bus (and any
    // intermediate group bus along the routing path) must stay
    // audible — they're part of the output topology, not competing
    // sources. The original v0.27.0 logic silenced every non-soloed
    // bus, which zeroed the master's output and produced total
    // silence even though the soloed child had written audio into
    // the master's input. This mask carves out the "stays audible"
    // set so the gating logic can leave the output path intact.
    //
    // Bit i is set iff buses_[i] is soloed OR has a descendant that
    // is soloed. When no bus is soloed, the mask is zero (== "no
    // solo mode active").
    //
    // kMaxBuses is 32 (include/audio_engine/bus.h); 64-bit mask has
    // headroom. If kMaxBuses ever exceeds 64, swap for an array.
    uint64_t ComputeSoloChainMask() const noexcept;

    // Look up a bus's proximity curve. Returns nullptr if the bus has no
    // proximity curve enabled.
    const ProximityCurve* ProximityCurveFor(BusId id) const noexcept;

private:
    struct Bus {
        BusId    id            = kInvalidBusId;
        uint32_t parentIndex   = kInvalidIndex;
        bool     silent        = false;
        ProximityCurve proximityCurve;
        std::atomic<float>     outputGainLinear{1.0f};

        // v0.24.0: copy of BusConfig::debugName (16 chars + NUL) so the
        // mixer dock can show human-readable names without a config
        // round-trip. Set once at Build time, read-only thereafter.
        char     debugName[16] = {0};

        // v0.24.0: peak abs sample observed in this bus's output buffer
        // since the last ReadAndResetBusPeakLinear call. Render-thread
        // writer (CapturePeakLinear), control-thread reader. The CAS
        // loop in CapturePeakLinear ensures max-wins under contention
        // (rare but possible if multiple render callbacks fire between
        // reader polls — won't happen with a single audio thread but
        // we play it safe). 0.0f when no audio has flowed yet.
        std::atomic<float>     peakSinceLastReadLinear{0.0f};

        // v0.27.0: per-bus runtime state for mute / solo / effect-bypass.
        // Atomic so the control thread can write directly (no command
        // queue needed — these are simple toggles with no per-sample
        // smoothing required). Render thread reads each at the top of
        // its bus iteration in AudioMixer::RunBusGraph.
        //
        // Semantics (matches DAW convention):
        //   - muted=true → output is zeroed before peak capture and
        //     parent routing
        //   - soloed=true → if ANY bus has soloed=true, all buses with
        //     soloed=false are silenced. Solo wins over mute (a bus
        //     that's both muted and soloed plays).
        //   - effectsBypassed=true → skip the effect chain entirely;
        //     output ends up as a copy of input. Mute/solo still apply
        //     after bypass.
        //
        // All three reset to false at AudioRuntime startup (i.e. F5);
        // session-only state, not persisted to config.json. Persistence
        // is the domain of Phase 3.3d.
        std::atomic<bool>      muted{false};
        std::atomic<bool>      soloed{false};
        std::atomic<bool>      effectsBypassed{false};

        // Hot-path mixing buffers. 64-byte aligned so cache-line-straddle
        // costs and AVX/AVX-512 unaligned-load penalties don't apply when
        // the compiler is targeting AVX. Move-only and crucially does NOT
        // expose `.push_back` / `.resize` / `.reserve` — these buffers
        // are sized once at BuildGraph time and must never grow.
        audio::util::AlignedFloatBuffer input;
        audio::util::AlignedFloatBuffer output;

        std::vector<std::unique_ptr<IDspEffect>> effects;
        std::vector<uint32_t>                    sidechainSourceIndex; // per effect
    };

    AudioResult ValidateAndBuildBuses(const BusGraphConfig& cfg);
    AudioResult BuildEffectsForBus(Bus& bus, const BusConfig& cfg);
    AudioResult BuildRenderOrder();

    std::vector<std::unique_ptr<Bus>> buses_;
    std::vector<uint32_t> renderOrder_;
    uint32_t              masterIndex_ = kInvalidIndex;

    uint32_t sampleRate_ = 48000;
    uint32_t channels_   = 2;
    uint32_t maxFrames_  = 0;

    CategoryBusMap categoryMap_;
};

} // namespace audio

#endif // AUDIO_ENGINE_MIXER_BUS_GRAPH_H
