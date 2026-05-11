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

    // ---- Control-thread parameter writes ---------------------------------

    AudioResult SetBusOutputGainDb(BusId id, float gainDb) noexcept;

    // ---- Diagnostics ------------------------------------------------------

    uint32_t Channels()    const noexcept { return channels_; }
    uint32_t MaxFrames()   const noexcept { return maxFrames_; }
    uint32_t SampleRate()  const noexcept { return sampleRate_; }

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
