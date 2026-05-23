// audio_engine/dsp/master_control.h
//
// v0.63.0 — Phase 7 first cut: Master FX Lite.
//
// One effect, four internal stages, drop on the Master bus.
//
//   input ─[1]─[2]─[3]─[4]─> output
//          │   │   │   │
//          │   │   │   └── True-peak brickwall limiter (-1 dBTP default)
//          │   │   └────── Slow LUFS-target gain rider (~3 s time constant)
//          │   └────────── LUFS meter (read-only, telemetry tap)
//          └────────────── Glue compressor (low ratio, gentle, soft knee)
//
// Three jobs, ranked by "how badly does it sound without this":
//   1. Don't clip — true-peak limiter is the hard ceiling.
//   2. Stay loudness-consistent across content — gain rider toward
//      integrated LUFS target. Cinematics whisper at -22 LUFS;
//      combat blows up at -8 LUFS without this. Slow time constant
//      (3 s) preserves emotional hierarchy: short loud events still
//      feel loud relative to the average.
//   3. Glue disparate sources — gentle bus compression with soft
//      knee. Subtle. Not loudness-wars; just cohesion.
//
// Each stage is independently bypassable. Default config = all
// stages on with conservative settings; the "audibly nothing
// happened" target the design doc demands for the default profile.
//
// What this is NOT (deferred per the doc's 8-stage vision):
//   - Spectral fatigue suppression (Phase C). No FFT in v0.63.0.
//   - Dialogue spectral protection (Phase E). Existing per-bus
//     sidechain compressors cover the gross case.
//   - Platform translation (Phase F). Different deliverable.
//   - Transient protection (Phase G stage 7). The lookahead
//     limiter handles transient preservation by itself.
//
// Performance budget (Steam Deck-class CPU @ 48kHz, 512-frame
// callback, stereo):
//   Glue compressor: ~0.05% CPU
//   LUFS meter:      ~0.10% CPU
//   Gain rider:      ~0.01% CPU
//   True-peak limiter (4× oversampled): ~0.30% CPU
//   Total:           ~0.50% CPU
// Well under the doc's 1.5% master-chain budget.
//
// Threading: this is a render-thread effect like all the others.
// Process() must not allocate. OnParameter() is called from the
// game thread between Process() calls; values are snapshotted to
// per-buffer target fields and ramped internally (~20 ms time
// constant) to avoid clicks on parameter changes — matches the
// SaturationEffect Phase 3 smoothing pattern.

#ifndef AUDIO_ENGINE_DSP_MASTER_CONTROL_H
#define AUDIO_ENGINE_DSP_MASTER_CONTROL_H

#include "audio_engine/dsp/biquad_filter.h"   // K-weighting filter (LUFS)
#include "audio_engine/dsp/dsp_effect.h"

#include <array>
#include <cstdint>
#include <vector>

namespace audio {

// Config passed to MasterControlEffect at construction. All fields
// have sensible defaults: a freshly-constructed effect with default
// config is the "Standard FPS" preset (glue on, gain rider on,
// limiter on, LUFS target -16, ceiling -1 dBTP).
struct MasterControlConfig {
    // Stage enable flags. Default true for all three audible stages
    // (LUFS meter is always on; it's a read-only tap).
    bool glueEnabled    = true;
    bool riderEnabled   = true;
    bool limiterEnabled = true;

    // Stage 1: Glue compressor.
    //   thresholdDb: where the gentle compression starts (-12 dB default;
    //     well below program peaks, conservative).
    //   ratio: 2:1 default. Loud peaks come down ~3 dB at hot moments.
    //   attackMs / releaseMs: slow attack to let transients through
    //     (10 ms), slow release for smooth recovery (250 ms).
    //   kneeDb: soft knee width. 6 dB makes the onset gradual so the
    //     compressor feels invisible at normal levels.
    float glueThresholdDb = -12.0f;
    float glueRatio       = 2.0f;
    float glueAttackMs    = 10.0f;
    float glueReleaseMs   = 250.0f;
    float glueKneeDb      = 6.0f;
    float glueMakeupDb    = 0.0f;

    // Stage 3: Gain rider — slow envelope follower targeting
    // integrated LUFS.
    //   targetLufs: where the rider aims. -16 LUFS is the "Default"
    //     profile from the design doc Section 5 Phase B; matches
    //     typical game-content loudness expectations.
    //   timeConstantMs: how slow the rider responds. 3000 ms = 3 s
    //     time constant. Slow enough that short loud events (boss
    //     attacks, explosions) don't trigger it — the rider tracks
    //     averages, not transients.
    //   maxGainDb / minGainDb: clamps on how much the rider can
    //     boost or cut. Default ±6 dB. A wider clamp risks bringing
    //     up noise floor during silence; a narrower one fails to
    //     correct truly quiet content.
    //   freezeBelowLufs: if integrated LUFS drops more than this
    //     much below target (default -6 LU), rider freezes to
    //     prevent the "menu silence then huge push-up when gameplay
    //     starts" failure mode the doc Section 5 Phase B describes.
    float riderTargetLufs     = -16.0f;
    float riderTimeConstantMs = 3000.0f;
    float riderMaxGainDb      = +6.0f;
    float riderMinGainDb      = -6.0f;
    float riderFreezeBelowLufs = -6.0f;

    // Stage 4: True-peak brickwall limiter.
    //   ceilingDbtp: hard ceiling in dBTP (true-peak dB). -1 dBTP
    //     default matches broadcast / cert convention; allows
    //     downstream consumers (lossy codecs, DAC reconstruction)
    //     a 1 dB headroom against intersample peaks.
    //   releaseMs: how fast the limiter releases after a peak.
    //     50 ms default; faster releases pump audibly, slower
    //     leaves audible volume dips after loud transients.
    //   lookaheadMs: 5 ms default = 240 samples @ 48 kHz. Adds
    //     5 ms of latency to the master bus (one-time, at scene
    //     load, not per-effect chain). This is acceptable on
    //     master; would not be acceptable on per-emitter paths.
    float limiterCeilingDbtp = -1.0f;
    float limiterReleaseMs   = 50.0f;
    float limiterLookaheadMs = 5.0f;
};

// Real-time-readable telemetry written every Process() call. The
// game thread reads via GetParameter(); these param IDs are part
// of the MasterControl namespace below. Fields are plain float;
// reads/writes are atomic on x86/ARM for single-word floats so
// the worst case is reading a value one callback behind a
// concurrent write — fine for telemetry.
struct MasterControlTelemetry {
    float lufsShortTermDb = -100.0f;   // 400 ms EBU R128 window
    float lufsIntegratedDb = -100.0f;  // unbounded integration since last reset
    float peakDb           = -100.0f;  // sample-peak post-glue, pre-limiter
    float truePeakDbtp     = -100.0f;  // intersample peak post-limiter
    float gainReductionDb  = 0.0f;     // limiter GR in dB (negative or zero)
    float riderGainDb      = 0.0f;     // current gain rider applied gain
};

class MasterControlEffect final : public IDspEffect {
public:
    explicit MasterControlEffect(const MasterControlConfig& cfg);

    void Prepare(uint32_t sampleRate, uint32_t channels) override;
    void Process(float* output, uint32_t frames, uint32_t channels,
                 const float* sidechain,
                 uint32_t sidechainChannels) noexcept override;
    void OnParameter(uint16_t paramId, float value) noexcept override;
    uint16_t SidechainBusId() const noexcept override { return kInvalidBusId; }
    EffectKind Kind() const noexcept override;
    float GetParameter(uint16_t paramId) const noexcept override;

    // Telemetry read-out. Snapshot of the most recent Process()
    // call. Safe to call from any thread (single-word float reads
    // are atomic on supported platforms).
    MasterControlTelemetry GetTelemetry() const noexcept;

    // Reset the integrated-LUFS accumulator. Call when transitioning
    // between game states (menu → gameplay → cinematic) so the
    // integration window doesn't drag the rider's stable point
    // across the discontinuity. Safe to call from the game thread
    // between Process() calls.
    void ResetIntegratedLufs() noexcept;

private:
    // --- Glue compressor (Stage 1) -----------------------------------------
    // Purpose-built simple compressor with soft knee, no sidechain. Not the
    // existing CompressorEffect (which is over-featured for this role).
    void ProcessGlue(float* buf, uint32_t frames, uint32_t channels) noexcept;
    float glueEnvelope_ = 0.0f;     // running detector envelope (linear)

    // --- LUFS meter (Stage 2, read-only) ----------------------------------
    // EBU R128 K-weighting = pre-filter (~1681 Hz high-shelf +4 dB) followed
    // by RLB filter (high-pass 38 Hz). Both implemented as biquads. After
    // K-weighting we square-and-sum into rolling buffers for short-term
    // (400 ms / 19200 samples @ 48kHz) and integrated (unbounded) loudness.
    void UpdateLufsMeter(const float* buf, uint32_t frames, uint32_t channels) noexcept;
    BiquadFilterEffect kWeightShelf_{BiquadType::HighShelf, 1681.97f, 0.707f, 4.0f};
    BiquadFilterEffect kWeightHpf_{BiquadType::HighPass, 38.13f, 0.5f};
    // Short-term window: 400 ms power buffer; circular index.
    std::vector<float> stPowerBuf_;
    uint32_t stPowerIdx_ = 0;
    double stPowerSum_ = 0.0;
    // Integrated: unbounded sum + count. EBU R128 spec gates blocks
    // below an absolute and relative threshold; v0.63.0 simplification
    // omits gating (just simple integration). Acceptable for telemetry
    // use; cert-grade integration is v0.63.x territory.
    double integratedPowerSum_ = 0.0;
    uint64_t integratedSampleCount_ = 0;

    // --- Gain rider (Stage 3) ---------------------------------------------
    void ProcessGainRider(float* buf, uint32_t frames, uint32_t channels) noexcept;
    float riderGainLinear_ = 1.0f;   // current per-sample gain (smoothed)
    float riderTargetGain_ = 1.0f;   // target gain computed from LUFS error

    // --- True-peak limiter (Stage 4) --------------------------------------
    // Lookahead brickwall with 4× oversampled true-peak detection.
    // Lookahead buffer holds `lookaheadSamples_` frames of input;
    // we write current input to it, read what's `lookaheadSamples_`
    // frames behind, and apply gain reduction computed from the
    // detected peak in the lookahead window. This way a transient
    // entering the lookahead can shape the gain reduction envelope
    // BEFORE it hits the output, preserving the transient onset.
    void ProcessLimiter(float* buf, uint32_t frames, uint32_t channels) noexcept;
    void ProcessOversamplePeak(const float* in, uint32_t frames,
                                 uint32_t channels) noexcept;
    std::vector<float> lookaheadBuf_;   // ring buffer, channels * lookaheadSamples_
    uint32_t lookaheadSamples_ = 0;
    uint32_t lookaheadIdx_ = 0;
    float limiterEnvelope_ = 1.0f;      // gain currently applied (linear)
    float currentTruePeak_ = 0.0f;      // most recent oversampled peak (linear)
    float currentGainReductionDb_ = 0.0f;

    // --- Common state -----------------------------------------------------
    MasterControlConfig cfg_;
    uint32_t sampleRate_ = 48000;
    uint32_t channels_   = 2;

    // Telemetry: written by Process(), read by game thread.
    // Per-buffer aggregates — peak, true-peak, gain reduction —
    // stored as plain float (atomic-on-supported-platforms idiom).
    float telLufsShortTerm_ = -100.0f;
    float telLufsIntegrated_ = -100.0f;
    float telPeakDb_ = -100.0f;
    float telTruePeakDbtp_ = -100.0f;
    float telGainReductionDb_ = 0.0f;
    float telRiderGainDb_ = 0.0f;
};

// Param IDs for the MasterControl effect are declared in
// audio_engine/bus.h alongside the other effect kinds' param IDs.
// MasterControl owns the 30-59 block; see EffectParameter::MC_*.

}  // namespace audio

#endif  // AUDIO_ENGINE_DSP_MASTER_CONTROL_H
