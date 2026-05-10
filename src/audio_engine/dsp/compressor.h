// audio_engine/dsp/compressor.h
//
// Dynamic range compressor with sidechain support. Topology:
//
//   input ──> [optional sidechain HPF on detect path] ──> [envelope
//   follower (peak or RMS)] ──> [soft- or hard-knee gain computer] ──>
//   [range cap] ──> [makeup gain] ──> [dry/wet mix] ──> output
//
// The detection signal is the sidechain bus when one is configured
// (see `CompressorConfig::sidechainBus`), otherwise the input itself
// (self-sidechain). The optional HPF sits on the detection path only
// — it shapes what frequencies trigger compression without coloring
// the audio output.
//
// Every parameter is runtime-tunable via SetEffectParameter using the
// IDs in bus.h's EffectParameter namespace (Compressor_*).

#ifndef AUDIO_ENGINE_DSP_COMPRESSOR_H
#define AUDIO_ENGINE_DSP_COMPRESSOR_H

#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/bus.h"

namespace audio {

// Configuration bundle. Defaults match the legacy 4:1 hard-knee
// peak-detect compressor — i.e. v0.7 behavior — so a default-constructed
// CompressorConfig is the historical compressor.
struct CompressorConfig {
    float thresholdDb     = -20.0f;
    float ratio           = 4.0f;
    float attackMs        = 10.0f;
    float releaseMs       = 200.0f;
    float makeupDb        = 0.0f;
    BusId sidechainBus    = kInvalidBusId;
    // Tier-A additions (v0.8):
    float kneeWidthDb     = 0.0f;     // 0 = hard knee
    float mixRatio        = 1.0f;     // 1.0 = fully wet
    float maxReductionDb  = 60.0f;    // 60 dB ≈ unlimited
    float sidechainHpfHz  = 0.0f;     // 0 = bypass
    float holdMs          = 0.0f;     // 0 = no hold
    EffectConfig::CompressorDetectionMode detectionMode =
        EffectConfig::CompressorDetectionMode::Peak;
};

class CompressorEffect final : public IDspEffect {
public:
    explicit CompressorEffect(const CompressorConfig& cfg);

    void Prepare(uint32_t sampleRate, uint32_t channels) override;
    void Process(float* output, uint32_t frames, uint32_t channels,
                 const float* sidechain, uint32_t sidechainChannels) noexcept override;
    void OnParameter(uint16_t paramId, float value) noexcept override;

    uint16_t SidechainBusId() const noexcept override {
        return static_cast<uint16_t>(sidechainBus_);
    }

    // For diagnostics / unit tests.
    float CurrentEnvelopeDb()  const noexcept { return envelopeDb_; }
    float CurrentReductionDb() const noexcept { return lastReductionDb_; }

private:
    using DetectionMode = EffectConfig::CompressorDetectionMode;

    // Recompute attack/release smoothing coefficients and the
    // sidechain HPF coefficients. Cheap; called lazily.
    void RecomputeCoeffs() noexcept;

    // ---- Configuration (target values) ----
    float thresholdDb_;
    float ratio_;
    float attackMs_;
    float releaseMs_;
    float makeupDb_;
    BusId sidechainBus_;
    float kneeWidthDb_;
    float mixRatio_;
    float maxReductionDb_;
    float sidechainHpfHz_;
    float holdMs_;
    DetectionMode detectionMode_;

    // ---- Derived state ----
    uint32_t sampleRate_   = 48000;
    float    attackCoeff_  = 0.0f;
    float    releaseCoeff_ = 0.0f;
    bool     coeffDirty_   = true;

    // Hold counter — samples remaining where the envelope is held at
    // its current value despite peak < env. Recharged to
    // `holdSamples_` on every attack-stage frame.
    uint32_t holdSamples_           = 0;
    uint32_t holdSamplesRemaining_  = 0;

    // Envelope state in dB. RMS mode converts via 20*log10(sqrt(mean))
    // per frame, then smooths in dB with attack/release — keeps time-
    // constant semantics consistent across modes.
    float envelopeDb_      = -120.0f;
    float lastReductionDb_ = 0.0f;

    // ---- Inline mono biquad for sidechain HPF ----
    // Direct-form-II-transposed. Engaged only when
    // sidechainHpfHz_ > 0; otherwise bypassed. The detection signal
    // is summed to mono before the filter / detector — standard
    // shape for sidechain HPFs and avoids per-channel state.
    float scB0_ = 1.0f, scB1_ = 0.0f, scB2_ = 0.0f;
    float scA1_ = 0.0f, scA2_ = 0.0f;
    float scZ1_ = 0.0f, scZ2_ = 0.0f;
};

} // namespace audio

#endif // AUDIO_ENGINE_DSP_COMPRESSOR_H
