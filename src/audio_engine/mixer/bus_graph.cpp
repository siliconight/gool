// audio_engine/mixer/bus_graph.cpp

#include "audio_engine/mixer/bus_graph.h"

#include "audio_engine/dsp/gain_effect.h"
#include "audio_engine/dsp/biquad_filter.h"
#include "audio_engine/dsp/compressor.h"
#include "audio_engine/dsp/reverb_effect.h"
#include "audio_engine/dsp/saturation_effect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {

// v0.24.1: MSVC fires C4996 ("This function or variable may be
// unsafe") on std::strncpy and with /WX (warnings-as-errors, set
// unconditionally in CMakeLists.txt for the audio_engine target)
// that's a hard error. So we don't use strncpy.
//
// v0.24.2: the previous "manual null-terminated copy in a single
// loop" version tripped cppcheck's arrayIndexOutOfBoundsCond with
// a false positive: cppcheck saw the call site `CopyBoundedString(
// b->debugName, 16, "Master")` and inferred "src is 8 bytes, the
// condition `i+1<dstSize` allows i up to 14, therefore src[i] could
// be out of bounds." That's only true if the short-circuit
// `src[i] != '\0'` failed to terminate the loop early, which it
// always does for null-terminated strings. cppcheck doesn't trace
// that interaction.
//
// The fix is a two-phase implementation: first determine srclen
// using strnlen (bounded, available via <cstring> on all three
// platforms — glibc, libc++, MSVC CRT — though not in the std::
// namespace since it's POSIX, not ISO C), then memcpy + null-
// terminate. The bound on srclen is visible at the subsequent
// memcpy and dst[srclen] write, so cppcheck can prove the writes
// stay in dst[0..dstSize-1].
//
// Used for copying BusConfig::debugName into the runtime Bus's
// debugName field at Build time. Both fields are char[16]; src may
// be a string literal ("Master") or another char[16] from BusConfig.
void CopyBoundedString(char* dst, std::size_t dstSize,
                        const char* src) noexcept {
    if (dstSize == 0) return;
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    const std::size_t srclen = strnlen(src, dstSize - 1);
    std::memcpy(dst, src, srclen);
    dst[srclen] = '\0';
}

} // anonymous namespace

namespace audio {

namespace {

float DbToLinearLocal(float dB) noexcept {
    return std::pow(10.0f, dB * 0.05f);
}

} // namespace

AudioResult BusGraph::Build(const BusGraphConfig& cfg,
                              uint32_t              sampleRate,
                              uint32_t              channels,
                              uint32_t              maxFrames) {
    sampleRate_ = sampleRate;
    channels_   = channels;
    maxFrames_  = maxFrames;
    categoryMap_ = cfg.categoryMap;

    if (auto rc = ValidateAndBuildBuses(cfg); rc != AudioResult::Success) return rc;
    if (auto rc = BuildRenderOrder();        rc != AudioResult::Success) return rc;

    // Bus input/output buffers, pre-sized to the worst-case render
    // block (maxFrames * channels floats each).
    //
    // IMPORTANT: `.assign(N, 0.0f)` writes a value to every byte of
    // each buffer at init time. This is load-bearing for real-time
    // safety — it forces the OS to back every page with real RAM
    // rather than a copy-on-write zero page. Without it, the first
    // render callback would page-fault into every buffer as it writes,
    // potentially blowing the audio callback's deadline. Don't change
    // to `reserve(N)` + later `resize(N)` (which default-constructs
    // floats without necessarily touching every page).
    for (const auto& bp : buses_) {
        bp->input.assign(static_cast<size_t>(maxFrames) * channels, 0.0f);
        bp->output.assign(static_cast<size_t>(maxFrames) * channels, 0.0f);
    }

    for (uint32_t i = 0; i < cfg.busCount; ++i) {
        const BusConfig& bcfg = cfg.buses[i];
        const uint32_t   idx  = IndexOf(bcfg.id);
        if (idx == kInvalidIndex) continue;
        if (auto rc = BuildEffectsForBus(*buses_[idx], bcfg); rc != AudioResult::Success) {
            return rc;
        }
    }

    for (const auto& bp : buses_) {
        for (auto& fx : bp->effects) {
            fx->Prepare(sampleRate_, channels_);
        }
    }

    return AudioResult::Success;
}

AudioResult BusGraph::ValidateAndBuildBuses(const BusGraphConfig& cfg) {
    buses_.clear();

    if (cfg.busCount == 0) {
        auto b = std::make_unique<Bus>();
        b->id            = kBusMaster;
        b->parentIndex   = kInvalidIndex;
        b->silent        = false;
        b->outputGainLinear.store(1.0f, std::memory_order_relaxed);
        // v0.24.0: synthetic master gets the canonical "Master" name so
        // the mixer dock has something to display even when no config
        // was loaded.
        CopyBoundedString(b->debugName, sizeof(b->debugName), "Master");
        buses_.push_back(std::move(b));
        masterIndex_ = 0;
        return AudioResult::Success;
    }

    if (cfg.busCount > kMaxBuses) return AudioResult::InvalidArgument;

    std::unordered_set<BusId> seenIds;
    bool hasMaster = false;
    for (uint32_t i = 0; i < cfg.busCount; ++i) {
        const BusId id = cfg.buses[i].id;
        if (id == kInvalidBusId) return AudioResult::InvalidArgument;
        if (!seenIds.insert(id).second) return AudioResult::InvalidArgument;
        if (id == kBusMaster) hasMaster = true;
    }
    if (!hasMaster) return AudioResult::InvalidArgument;

    buses_.reserve(cfg.busCount);
    for (uint32_t i = 0; i < cfg.busCount; ++i) {
        const BusConfig& bcfg = cfg.buses[i];
        if (bcfg.id != kBusMaster) continue;
        auto b = std::make_unique<Bus>();
        b->id          = bcfg.id;
        b->parentIndex = kInvalidIndex;
        b->silent      = bcfg.silent;
        b->proximityCurve = bcfg.proximityCurve;
        b->outputGainLinear.store(DbToLinearLocal(bcfg.outputGainDb),
                                   std::memory_order_relaxed);
        // v0.24.0: carry the human-readable name through to the runtime
        // Bus so the mixer dock can display it without a config lookup.
        CopyBoundedString(b->debugName, sizeof(b->debugName), bcfg.debugName);
        masterIndex_ = static_cast<uint32_t>(buses_.size());
        buses_.push_back(std::move(b));
        break;
    }
    for (uint32_t i = 0; i < cfg.busCount; ++i) {
        const BusConfig& bcfg = cfg.buses[i];
        if (bcfg.id == kBusMaster) continue;
        auto b = std::make_unique<Bus>();
        b->id          = bcfg.id;
        b->silent      = bcfg.silent;
        b->proximityCurve = bcfg.proximityCurve;
        b->outputGainLinear.store(DbToLinearLocal(bcfg.outputGainDb),
                                   std::memory_order_relaxed);
        CopyBoundedString(b->debugName, sizeof(b->debugName), bcfg.debugName);
        buses_.push_back(std::move(b));
    }

    for (const auto& bp : buses_) {
        if (bp->id == kBusMaster) { bp->parentIndex = kInvalidIndex; continue; }
        const BusConfig* bcfg = nullptr;
        for (uint32_t i = 0; i < cfg.busCount; ++i) {
            if (cfg.buses[i].id == bp->id) { bcfg = &cfg.buses[i]; break; }
        }
        if (!bcfg) return AudioResult::InternalError;
        const uint32_t parentIdx = IndexOf(bcfg->parent);
        if (parentIdx == kInvalidIndex) return AudioResult::InvalidArgument;
        bp->parentIndex = parentIdx;
    }

    for (uint32_t i = 0; i < buses_.size(); ++i) {
        uint32_t walker = buses_[i]->parentIndex;
        uint32_t hops   = 0;
        while (walker != kInvalidIndex) {
            if (walker == i) return AudioResult::InvalidArgument;     // cycle
            if (++hops > buses_.size()) return AudioResult::InvalidArgument;
            walker = buses_[walker]->parentIndex;
        }
    }

    return AudioResult::Success;
}

AudioResult BusGraph::BuildEffectsForBus(Bus& bus, const BusConfig& cfg) {
    if (cfg.effectCount > kMaxEffectsPerBus) return AudioResult::InvalidArgument;
    bus.effects.reserve(cfg.effectCount);
    bus.sidechainSourceIndex.reserve(cfg.effectCount);

    for (uint32_t i = 0; i < cfg.effectCount; ++i) {
        const EffectConfig& ec = cfg.effects[i];
        std::unique_ptr<IDspEffect> fx;
        uint32_t sidechainIdx = kInvalidIndex;

        switch (ec.kind) {
            case EffectKind::None:
                continue;
            case EffectKind::Gain:
                fx = std::make_unique<GainEffect>(ec.gainDb);
                break;
            case EffectKind::BiquadFilter:
                fx = std::make_unique<BiquadFilterEffect>(
                    ec.biquadType, ec.biquadCutoffHz, ec.biquadQ, ec.biquadGainDb);
                break;
            case EffectKind::Compressor: {
                CompressorConfig cc;
                cc.thresholdDb     = ec.compressorThresholdDb;
                cc.ratio           = ec.compressorRatio;
                cc.attackMs        = ec.compressorAttackMs;
                cc.releaseMs       = ec.compressorReleaseMs;
                cc.makeupDb        = ec.compressorMakeupDb;
                cc.sidechainBus    = ec.compressorSidechainBus;
                // Tier-A (v0.8) parameters:
                cc.kneeWidthDb     = ec.compressorKneeWidthDb;
                cc.mixRatio        = ec.compressorMixRatio;
                cc.maxReductionDb  = ec.compressorMaxReductionDb;
                cc.sidechainHpfHz  = ec.compressorSidechainHpfHz;
                cc.holdMs          = ec.compressorHoldMs;
                cc.detectionMode   = ec.compressorDetectionMode;
                fx = std::make_unique<CompressorEffect>(cc);
                if (ec.compressorSidechainBus != kInvalidBusId) {
                    const uint32_t scIdx = IndexOf(ec.compressorSidechainBus);
                    if (scIdx == kInvalidIndex) return AudioResult::InvalidArgument;
                    sidechainIdx = scIdx;
                }
            } break;
            case EffectKind::Reverb:
                fx = std::make_unique<ReverbEffect>(
                    ec.reverbRoomSize,
                    ec.reverbDamping,
                    ec.reverbWetGainDb);
                break;
            case EffectKind::Saturation: {
                SaturationConfig sc;
                sc.drive      = ec.saturationDrive;
                sc.mix        = ec.saturationMix;
                sc.outputGain = ec.saturationOutputGain;
                sc.bias       = ec.saturationBias;
                fx = std::make_unique<SaturationEffect>(sc);
            } break;
        }

        bus.effects.push_back(std::move(fx));
        bus.sidechainSourceIndex.push_back(sidechainIdx);
    }
    return AudioResult::Success;
}

AudioResult BusGraph::BuildRenderOrder() {
    const uint32_t N = static_cast<uint32_t>(buses_.size());
    std::vector<std::vector<uint32_t>> outEdges(N);
    std::vector<uint32_t>              inDegree(N, 0);

    for (uint32_t i = 0; i < N; ++i) {
        const Bus& b = *buses_[i];
        if (b.parentIndex != kInvalidIndex) {
            outEdges[i].push_back(b.parentIndex);
            inDegree[b.parentIndex]++;
        }
        for (uint32_t scIdx : b.sidechainSourceIndex) {
            if (scIdx != kInvalidIndex) {
                outEdges[scIdx].push_back(i);
                inDegree[i]++;
            }
        }
    }

    renderOrder_.clear();
    renderOrder_.reserve(N);
    std::vector<uint32_t> ready;
    ready.reserve(N);
    for (uint32_t i = 0; i < N; ++i) {
        if (inDegree[i] == 0) ready.push_back(i);
    }
    while (!ready.empty()) {
        const uint32_t v = ready.back();
        ready.pop_back();
        renderOrder_.push_back(v);
        for (uint32_t w : outEdges[v]) {
            if (--inDegree[w] == 0) ready.push_back(w);
        }
    }
    if (renderOrder_.size() != N) return AudioResult::InvalidArgument;
    return AudioResult::Success;
}

uint32_t BusGraph::IndexOf(BusId id) const noexcept {
    for (uint32_t i = 0; i < buses_.size(); ++i) {
        if (buses_[i]->id == id) return i;
    }
    return kInvalidIndex;
}

uint32_t BusGraph::IndexForCategory(AudioCategory cat) const noexcept {
    BusId id = kBusMaster;
    switch (cat) {
        case AudioCategory::Music:    id = categoryMap_.music;    break;
        case AudioCategory::Voice:    id = categoryMap_.voice;    break;
        case AudioCategory::SFX:      id = categoryMap_.sfx;      break;
        case AudioCategory::Ambience: id = categoryMap_.ambience; break;
        case AudioCategory::UI:       id = categoryMap_.ui;       break;
        case AudioCategory::Dialogue: id = categoryMap_.dialogue; break;
        case AudioCategory::Count:    break;
    }
    const uint32_t idx = IndexOf(id);
    return (idx != kInvalidIndex) ? idx : masterIndex_;
}

void BusGraph::ClearAllInputBuffers(uint32_t frames, uint32_t channels) noexcept {
    const size_t bytes = static_cast<size_t>(frames) * channels * sizeof(float);
    for (const auto& bp : buses_) {
        std::memset(bp->input.data(), 0, bytes);
    }
}

IDspEffect* BusGraph::EffectAt(uint32_t busIndex, uint32_t effectIndex) noexcept {
    if (busIndex >= buses_.size()) return nullptr;
    auto& effs = buses_[busIndex]->effects;
    if (effectIndex >= effs.size()) return nullptr;
    return effs[effectIndex].get();
}

uint32_t BusGraph::EffectCount(uint32_t busIndex) const noexcept {
    if (busIndex >= buses_.size()) return 0;
    return static_cast<uint32_t>(buses_[busIndex]->effects.size());
}

uint32_t BusGraph::SidechainSourceIndex(uint32_t busIndex, uint32_t effectIndex) const noexcept {
    if (busIndex >= buses_.size()) return kInvalidIndex;
    const auto& v = buses_[busIndex]->sidechainSourceIndex;
    if (effectIndex >= v.size()) return kInvalidIndex;
    return v[effectIndex];
}

AudioResult BusGraph::SetBusOutputGainDb(BusId id, float gainDb) noexcept {
    const uint32_t idx = IndexOf(id);
    if (idx == kInvalidIndex) return AudioResult::InvalidArgument;
    buses_[idx]->outputGainLinear.store(DbToLinearLocal(gainDb),
                                          std::memory_order_relaxed);
    return AudioResult::Success;
}

const ProximityCurve* BusGraph::ProximityCurveFor(BusId id) const noexcept {
    const uint32_t idx = IndexOf(id);
    if (idx == kInvalidIndex) return nullptr;
    const auto& pc = buses_[idx]->proximityCurve;
    return pc.enabled ? &pc : nullptr;
}

// v0.24.0: per-bus metering. Render thread writes via CapturePeakLinear
// after each bus's effect chain runs; control thread reads via
// ReadAndResetBusPeakLinear from the editor's mixer dock at ~30 Hz.
//
// Atomicity model: peakSinceLastReadLinear is updated with a CAS loop
// that keeps the maximum value. With the current single-audio-thread
// design the CAS loop is overkill (only one writer), but we use it
// anyway so future multi-threaded mixers don't need to revisit this.
//
// Floating-point note: comparisons assume IEEE 754 ordering. Audio
// samples are bounded |x| ≤ ~16 (post-effect dynamic range), well
// within the comparable range — no NaN/Inf paths to worry about
// (effects sanitize their own outputs upstream).

void BusGraph::CapturePeakLinear(uint32_t busIndex,
                                  const float* output,
                                  uint32_t     frames,
                                  uint32_t     channels) noexcept {
    if (busIndex >= buses_.size() || output == nullptr) return;
    const uint32_t total = frames * channels;
    if (total == 0) return;

    float observed = 0.0f;
    for (uint32_t i = 0; i < total; ++i) {
        const float a = std::fabs(output[i]);
        if (a > observed) observed = a;
    }

    // CAS-update: peak = max(peak, observed). With a single writer this
    // collapses to a load+store, but the loop is correct under contention
    // and the cost is negligible (rare contention, no allocations).
    std::atomic<float>& slot = buses_[busIndex]->peakSinceLastReadLinear;
    float prev = slot.load(std::memory_order_relaxed);
    while (observed > prev) {
        if (slot.compare_exchange_weak(prev, observed,
                                        std::memory_order_relaxed)) {
            break;
        }
        // prev was reloaded by compare_exchange_weak; loop checks again.
    }
}

float BusGraph::ReadAndResetBusPeakLinear(uint32_t busIndex) noexcept {
    if (busIndex >= buses_.size()) return 0.0f;
    // exchange returns the prior value AND atomically writes 0.0f, so
    // the reader observes peak-since-last-read and the writer's next
    // observation starts a fresh window. memory_order_acquire on the
    // exchange ensures we see all samples that wrote into that peak.
    return buses_[busIndex]->peakSinceLastReadLinear.exchange(
        0.0f, std::memory_order_acq_rel);
}

const char* BusGraph::BusName(uint32_t busIndex) const noexcept {
    if (busIndex >= buses_.size()) return "";
    return buses_[busIndex]->debugName;
}

} // namespace audio
