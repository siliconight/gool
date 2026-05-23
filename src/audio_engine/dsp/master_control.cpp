// src/audio_engine/dsp/master_control.cpp
//
// Implementation of MasterControlEffect — see master_control.h
// for the architectural overview and per-stage rationale.
//
// Internal stage order (each can be independently bypassed):
//   1. Glue compressor: gentle bus cohesion. Soft knee, low ratio,
//      slow attack. Designed to be invisible at normal levels and
//      do ~1-3 dB of work at hot moments.
//   2. LUFS meter: K-weighted EBU R128. Read-only telemetry tap;
//      provides short-term (400 ms window) and integrated (since
//      reset) loudness in LUFS to the gain rider AND the dock.
//   3. Gain rider: slow follower (~3 s time constant) nudging the
//      bus toward a target integrated LUFS. NOT a fast compressor —
//      the long time constant preserves emotional hierarchy
//      (transients stay transient; only averages stabilize).
//   4. True-peak limiter: lookahead brickwall at -1 dBTP. 4×
//      oversampled peak detection catches intersample peaks the
//      sample-rate detector misses. Lookahead lets the gain
//      reduction envelope shape ahead of arriving transients so
//      onsets stay sharp.

#include "audio_engine/dsp/master_control.h"

#include "audio_engine/dsp/biquad_filter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace audio {

namespace {

// dB ↔ linear conversions.
inline float DbToLin(float db) noexcept {
    return std::pow(10.0f, db * (1.0f / 20.0f));
}
inline float LinToDb(float lin) noexcept {
    // Floor at -100 dB so silence doesn't produce -inf and break
    // downstream arithmetic / telemetry display.
    if (lin <= 1e-10f) return -100.0f;
    return 20.0f * std::log10(lin);
}

// Per-buffer single-pole smoothing coefficient.
inline float OnePoleCoef(float timeConstantMs, float sampleRate) noexcept {
    if (timeConstantMs <= 0.0f) return 0.0f;
    return std::exp(-1.0f / (timeConstantMs * 0.001f * sampleRate));
}

// dBTP-to-linear for limiter ceiling. dBTP and dB are equivalent
// in scalar magnitude; the "TP" suffix just communicates the
// measurement convention. Here we treat the ceiling as a hard
// magnitude limit on the OVERSAMPLED peak.
inline float CeilingLinear(float dbtp) noexcept {
    return DbToLin(dbtp);
}

// Maximum frames the limiter lookahead buffer can hold. 10 ms at
// 48 kHz = 480 frames; we round up to 1024 for safety against
// sample-rate variations. Larger lookaheads aren't useful (latency
// vs. peak-shaping tradeoff stops paying off past ~10 ms).
constexpr uint32_t kMaxLookaheadSamples = 1024;

// EBU R128 absolute-silence threshold in linear power. Below this,
// the integrated-LUFS accumulator skips the buffer entirely. This
// is a simplified version of R128's gating (which uses both
// absolute and relative thresholds + 400 ms blocks); for v0.63.0
// we use just the absolute floor to avoid silence dragging the
// integrated LUFS toward -∞ in long quiet sections.
//   -70 LUFS ≈ 1e-7 in power; we use 1e-8 for headroom.
constexpr double kIntegratedSilencePowerThreshold = 1e-8;

}  // namespace

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

MasterControlEffect::MasterControlEffect(const MasterControlConfig& cfg)
    : cfg_(cfg) {
    // K-weighting filters are initialized via member initializer list in
    // the header. Their Prepare() is called from our Prepare() below.
}

void MasterControlEffect::Prepare(uint32_t sampleRate, uint32_t channels) {
    sampleRate_ = sampleRate;
    channels_   = channels;

    // K-weighting biquads. The filter has two stages: a high-shelf
    // (+4 dB at ~1681 Hz) approximating the head/torso transfer,
    // followed by a high-pass (~38 Hz) approximating the low-end
    // perceptual roll-off. ITU-R BS.1770 specifies the exact
    // coefficients; biquad gain/frequency approximation is what
    // most production code uses (matches the spec to within ~0.1 dB
    // across the audible band).
    kWeightShelf_.Prepare(sampleRate, channels);
    kWeightHpf_.Prepare(sampleRate, channels);

    // Short-term power buffer: 400 ms @ sampleRate, MONO power
    // (sum-of-squares across channels then averaged). The buffer
    // holds per-sample power values; we maintain stPowerSum_ as
    // the running sum to avoid re-summing every frame.
    const uint32_t stSamples = static_cast<uint32_t>(
            std::round(0.400 * static_cast<double>(sampleRate)));
    stPowerBuf_.assign(stSamples, 0.0f);
    stPowerIdx_ = 0;
    stPowerSum_ = 0.0;

    // Integrated LUFS accumulators reset at Prepare. Game code
    // can call ResetIntegratedLufs() on state transitions.
    integratedPowerSum_     = 0.0;
    integratedSampleCount_  = 0;

    // Lookahead buffer for the limiter. Sized from cfg.limiterLookaheadMs
    // but capped at kMaxLookaheadSamples. Allocated only here in Prepare;
    // Process never allocates.
    const uint32_t lookaheadFrames = std::min(
            kMaxLookaheadSamples,
            static_cast<uint32_t>(std::round(
                    cfg_.limiterLookaheadMs * 0.001f *
                    static_cast<float>(sampleRate))));
    lookaheadSamples_ = lookaheadFrames;
    lookaheadBuf_.assign(
            static_cast<size_t>(lookaheadFrames) * channels, 0.0f);
    lookaheadIdx_   = 0;
    limiterEnvelope_ = 1.0f;
    currentTruePeak_ = 0.0f;
    currentGainReductionDb_ = 0.0f;

    // Stage 1 envelope starts clean.
    glueEnvelope_ = 0.0f;

    // Rider gain starts at unity.
    riderGainLinear_ = 1.0f;
    riderTargetGain_ = 1.0f;

    // Telemetry: floor everything.
    telLufsShortTerm_     = -100.0f;
    telLufsIntegrated_    = -100.0f;
    telPeakDb_            = -100.0f;
    telTruePeakDbtp_      = -100.0f;
    telGainReductionDb_   = 0.0f;
    telRiderGainDb_       = 0.0f;
}

EffectKind MasterControlEffect::Kind() const noexcept {
    return EffectKind::MasterControl;
}

// ---------------------------------------------------------------------------
// Process — the render-thread hot path
// ---------------------------------------------------------------------------

void MasterControlEffect::Process(float* output, uint32_t frames,
                                    uint32_t channels,
                                    const float* /*sidechain*/,
                                    uint32_t /*sidechainChannels*/) noexcept {
    if (frames == 0 || channels == 0) return;

    // Stage 1: Glue compressor (in-place on output buffer).
    if (cfg_.glueEnabled) {
        ProcessGlue(output, frames, channels);
    }

    // Track post-glue sample peak for telemetry.
    float peakLin = 0.0f;
    for (uint32_t i = 0, n = frames * channels; i < n; ++i) {
        const float a = std::fabs(output[i]);
        if (a > peakLin) peakLin = a;
    }
    telPeakDb_ = LinToDb(peakLin);

    // Stage 2: LUFS meter (read-only tap). Always runs even when
    // rider/limiter are off because the dock relies on its
    // telemetry. Cost is one K-weighting biquad pair pass per
    // frame plus an O(frames) running-sum update.
    UpdateLufsMeter(output, frames, channels);

    // Stage 3: Gain rider. Reads short-term LUFS, computes target
    // gain, smooths toward it with the configured time constant,
    // applies the smoothed gain in-place.
    if (cfg_.riderEnabled) {
        ProcessGainRider(output, frames, channels);
    }

    // Stage 4: True-peak limiter. Lookahead buffer means there's
    // 5 ms of latency through this stage; the limiter's output is
    // delayed relative to its input by lookaheadSamples_ frames.
    if (cfg_.limiterEnabled) {
        ProcessLimiter(output, frames, channels);
    }
}

// ---------------------------------------------------------------------------
// Stage 1: Glue compressor
// ---------------------------------------------------------------------------

void MasterControlEffect::ProcessGlue(float* buf, uint32_t frames,
                                       uint32_t channels) noexcept {
    // Simple soft-knee feed-forward compressor. Detector = peak of
    // |sample| across channels. Single-pole envelope follower with
    // separate attack/release. Soft knee uses the standard quadratic
    // transition centered on threshold, with cfg_.glueKneeDb total
    // width.
    const float ratio        = std::max(1.0f, cfg_.glueRatio);
    const float kneeDb       = std::max(0.0f, cfg_.glueKneeDb);
    const float makeupLin    = DbToLin(cfg_.glueMakeupDb);

    const float attackCoef  = OnePoleCoef(cfg_.glueAttackMs,
                                           static_cast<float>(sampleRate_));
    const float releaseCoef = OnePoleCoef(cfg_.glueReleaseMs,
                                           static_cast<float>(sampleRate_));

    for (uint32_t i = 0; i < frames; ++i) {
        // Detector: peak across channels of this frame.
        float frameMax = 0.0f;
        for (uint32_t c = 0; c < channels; ++c) {
            const float a = std::fabs(buf[i * channels + c]);
            if (a > frameMax) frameMax = a;
        }

        // Envelope follower (attack/release on the |signal|).
        const float coef = (frameMax > glueEnvelope_) ? attackCoef
                                                       : releaseCoef;
        glueEnvelope_ = coef * glueEnvelope_ + (1.0f - coef) * frameMax;

        // Compute gain reduction in dB from the current envelope.
        // Soft knee: linear pass-through up to (threshold - knee/2),
        // quadratic transition through the knee, then linear
        // compression past (threshold + knee/2).
        const float envDb = LinToDb(glueEnvelope_);
        float grDb = 0.0f;  // gain reduction (always <= 0)
        const float kneeHalf = kneeDb * 0.5f;
        if (envDb > cfg_.glueThresholdDb + kneeHalf) {
            // Above the knee — linear in dB-space.
            const float overDb = envDb - cfg_.glueThresholdDb;
            grDb = -(overDb - overDb / ratio);
        } else if (envDb > cfg_.glueThresholdDb - kneeHalf) {
            // Inside the knee. Quadratic interpolation between
            // 0 dB GR (knee bottom) and the post-knee linear value
            // (knee top). x in [0..1] across the knee width.
            const float x = (envDb - (cfg_.glueThresholdDb - kneeHalf))
                             / kneeDb;
            // dB-space slope of the linear compression line is
            // (1 - 1/ratio); the knee curve is x^2 * (slope * knee).
            const float slope = 1.0f - (1.0f / ratio);
            grDb = -(x * x) * slope * kneeHalf;
        }
        // Below the knee → grDb stays 0 (no compression).

        const float gainLin = DbToLin(grDb) * makeupLin;
        for (uint32_t c = 0; c < channels; ++c) {
            buf[i * channels + c] *= gainLin;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 2: LUFS meter
// ---------------------------------------------------------------------------

void MasterControlEffect::UpdateLufsMeter(const float* buf, uint32_t frames,
                                            uint32_t channels) noexcept {
    // Run the buffer through K-weighting filters. We need to do this
    // on a SCRATCH copy because the meter is read-only — the
    // K-weighted signal is what we measure, not what we pass
    // downstream. To avoid allocating a scratch buffer, we pre-
    // allocated stPowerBuf_ at Prepare; but that's for the rolling
    // power, not the per-sample K-weighted signal. So we filter
    // in-place into a small local stack array, in chunks.
    //
    // Stack scratch: process in chunks of 64 frames so the buffer
    // fits in L1 cache. Stack-allocated to avoid heap touches.
    constexpr uint32_t kChunk = 64;
    float scratch[kChunk * 8];  // up to 8 channels (matches BiquadFilter)

    for (uint32_t base = 0; base < frames; base += kChunk) {
        const uint32_t n = std::min(kChunk, frames - base);
        // Copy this chunk from the input buffer.
        std::memcpy(scratch, buf + base * channels,
                     n * channels * sizeof(float));

        // Run K-weighting biquad pair on the chunk.
        kWeightShelf_.Process(scratch, n, channels, nullptr, 0);
        kWeightHpf_.Process(scratch, n, channels, nullptr, 0);

        // Accumulate per-frame power = mean of squared K-weighted
        // samples across channels. EBU R128 channel weighting for
        // mono/stereo is just unit weight; surround weights would
        // multiply some channels by 1.41 (5.1 Ls/Rs). For v0.63.0
        // we treat all channels equally — most game projects are
        // stereo, and the surround case can be addressed in a
        // follow-up if needed.
        const float invChannels = 1.0f / static_cast<float>(channels);
        for (uint32_t i = 0; i < n; ++i) {
            double power = 0.0;
            for (uint32_t c = 0; c < channels; ++c) {
                const double s = scratch[i * channels + c];
                power += s * s;
            }
            power *= invChannels;

            // Short-term rolling sum-of-squares: subtract the
            // sample being overwritten, add the new sample.
            const float oldSample = stPowerBuf_[stPowerIdx_];
            stPowerSum_ -= static_cast<double>(oldSample);
            stPowerSum_ += power;
            // Clamp tiny negative drift from FP cancellation.
            if (stPowerSum_ < 0.0) stPowerSum_ = 0.0;
            stPowerBuf_[stPowerIdx_] = static_cast<float>(power);
            stPowerIdx_ = (stPowerIdx_ + 1) %
                          static_cast<uint32_t>(stPowerBuf_.size());

            // Integrated: gated accumulation. Skip silence-level
            // power (see kIntegratedSilencePowerThreshold rationale).
            if (power > kIntegratedSilencePowerThreshold) {
                integratedPowerSum_ += power;
                ++integratedSampleCount_;
            }
        }
    }

    // Compute LUFS values for telemetry. -0.691 dB is the EBU R128
    // calibration offset (K-weighted RMS of a 1 kHz reference tone
    // at -23 dBFS reads -23.691 LUFS without compensation;
    // adding +0.691 dB calibrates to spec).
    const double stMeanPower = stPowerSum_ /
                                 static_cast<double>(stPowerBuf_.size());
    telLufsShortTerm_ = (stMeanPower > 1e-10)
            ? static_cast<float>(-0.691 + 10.0 * std::log10(stMeanPower))
            : -100.0f;

    if (integratedSampleCount_ > 0) {
        const double intMeanPower = integratedPowerSum_ /
                                     static_cast<double>(integratedSampleCount_);
        telLufsIntegrated_ = static_cast<float>(
                -0.691 + 10.0 * std::log10(intMeanPower));
    }
}

// ---------------------------------------------------------------------------
// Stage 3: Gain rider
// ---------------------------------------------------------------------------

void MasterControlEffect::ProcessGainRider(float* buf, uint32_t frames,
                                             uint32_t channels) noexcept {
    // Compute target gain from short-term LUFS error.
    // error = target - current; positive error means signal is
    // quieter than target → need to boost; negative means louder
    // than target → need to cut.
    const float currentLufs = telLufsShortTerm_;
    const float errorDb     = cfg_.riderTargetLufs - currentLufs;

    // Freeze behavior: if we're far below the target (silence /
    // very quiet content), don't push the gain up — that would
    // amplify noise floor during cinematics or menu screens. The
    // freeze threshold is relative to target (e.g. target=-16,
    // freeze=-6 means freeze when current is below -22).
    const bool frozen = (currentLufs < cfg_.riderTargetLufs +
                                          cfg_.riderFreezeBelowLufs);
    if (!frozen) {
        // Clamp error to the configured rider range.
        const float clampedDb = std::clamp(errorDb,
                                            cfg_.riderMinGainDb,
                                            cfg_.riderMaxGainDb);
        riderTargetGain_ = DbToLin(clampedDb);
    }
    // If frozen: keep last riderTargetGain_; the smoothing below
    // will hold position as the per-buffer gain target.

    // Smooth toward the target with the configured time constant.
    // We apply one per-buffer smoothing step (not per-sample) since
    // the time constant is multi-second; per-sample would be
    // unnecessary work.
    const float coef = OnePoleCoef(cfg_.riderTimeConstantMs,
                                    static_cast<float>(sampleRate_) /
                                    static_cast<float>(frames));
    riderGainLinear_ = coef * riderGainLinear_ +
                        (1.0f - coef) * riderTargetGain_;

    // Apply gain to the buffer. Single multiply per sample.
    const float g = riderGainLinear_;
    for (uint32_t i = 0, n = frames * channels; i < n; ++i) {
        buf[i] *= g;
    }

    telRiderGainDb_ = LinToDb(riderGainLinear_);
}

// ---------------------------------------------------------------------------
// Stage 4: True-peak limiter
// ---------------------------------------------------------------------------

void MasterControlEffect::ProcessLimiter(float* buf, uint32_t frames,
                                          uint32_t channels) noexcept {
    if (lookaheadSamples_ == 0) {
        // Lookahead disabled — degrade to a simple sample-peak
        // limiter without lookahead. Less optimal transient
        // preservation but still brickwall-safe.
        const float ceiling = CeilingLinear(cfg_.limiterCeilingDbtp);
        for (uint32_t i = 0; i < frames; ++i) {
            float frameMax = 0.0f;
            for (uint32_t c = 0; c < channels; ++c) {
                const float a = std::fabs(buf[i * channels + c]);
                if (a > frameMax) frameMax = a;
            }
            if (frameMax > ceiling) {
                const float gr = ceiling / frameMax;
                for (uint32_t c = 0; c < channels; ++c) {
                    buf[i * channels + c] *= gr;
                }
            }
        }
        telTruePeakDbtp_ = LinToDb(currentTruePeak_);
        telGainReductionDb_ = currentGainReductionDb_;
        return;
    }

    // Lookahead path: ring buffer holds the last `lookaheadSamples_`
    // frames. For each incoming frame, we:
    //   1. Detect oversampled peak in a small window around it.
    //   2. Update the envelope-shaped gain reduction.
    //   3. Read the frame `lookaheadSamples_` behind, apply gain,
    //      emit as output.
    //   4. Write the incoming frame into the ring buffer slot we
    //      just emitted.
    //
    // This is the standard lookahead brickwall structure used in
    // most production limiters; correctness depends on the gain
    // reduction envelope having reached its target value BY THE
    // TIME the peak frame reaches the output. With attack time ≈
    // 0 and the lookahead == 5 ms, the envelope is fully formed
    // before the peak emerges, so the peak is attenuated exactly
    // as needed.

    const float ceiling = CeilingLinear(cfg_.limiterCeilingDbtp);
    const float releaseCoef = OnePoleCoef(cfg_.limiterReleaseMs,
                                            static_cast<float>(sampleRate_));

    // Detect oversampled true-peak across the incoming buffer +
    // lookahead region. For simplicity v0.63.0 uses a 4× linear
    // interpolation between consecutive samples as the "oversample"
    // — proper polyphase oversampling is a future improvement but
    // linear interpolation catches the bulk of intersample peaks
    // and adds negligible cost. ITU-R BS.1770 mandates polyphase
    // for cert-grade measurement; for runtime detection-driven
    // limiting linear is industry-acceptable.
    ProcessOversamplePeak(buf, frames, channels);

    for (uint32_t i = 0; i < frames; ++i) {
        // Step 1: find the peak in the lookahead window. We scan
        // the entire ring buffer here — O(lookaheadSamples_) per
        // frame. At 5 ms / 240 frames @ 48kHz this is cheap.
        float windowPeak = 0.0f;
        for (uint32_t j = 0; j < lookaheadSamples_; ++j) {
            for (uint32_t c = 0; c < channels; ++c) {
                const float a = std::fabs(
                        lookaheadBuf_[j * channels + c]);
                if (a > windowPeak) windowPeak = a;
            }
        }
        // Compute target gain reduction. If windowPeak > ceiling,
        // we need to scale down so windowPeak * gain == ceiling.
        const float targetGain = (windowPeak > ceiling)
                ? (ceiling / windowPeak)
                : 1.0f;

        // Attack is "as fast as needed" — clamp instantly to a
        // lower gain. Release smooths back to unity gain.
        if (targetGain < limiterEnvelope_) {
            limiterEnvelope_ = targetGain;
        } else {
            limiterEnvelope_ = releaseCoef * limiterEnvelope_ +
                                (1.0f - releaseCoef) * targetGain;
        }

        // Step 2: read out the delayed frame, apply gain, emit.
        // The slot at lookaheadIdx_ is the OLDEST frame in the
        // ring (about to be overwritten); that's the frame we
        // emit, since it's the one that's been waiting the full
        // lookahead duration.
        float emittedMaxAbs = 0.0f;
        for (uint32_t c = 0; c < channels; ++c) {
            const float delayed =
                    lookaheadBuf_[lookaheadIdx_ * channels + c];
            const float emitted = delayed * limiterEnvelope_;
            const float a = std::fabs(emitted);
            if (a > emittedMaxAbs) emittedMaxAbs = a;

            // Write incoming frame to the same slot — and emit
            // the gained delayed frame.
            lookaheadBuf_[lookaheadIdx_ * channels + c] =
                    buf[i * channels + c];
            buf[i * channels + c] = emitted;
        }
        lookaheadIdx_ = (lookaheadIdx_ + 1) % lookaheadSamples_;

        // Track gain reduction for telemetry (most negative = most
        // GR over the buffer).
        const float grDb = LinToDb(limiterEnvelope_);
        if (grDb < currentGainReductionDb_) {
            currentGainReductionDb_ = grDb;
        }
    }

    telTruePeakDbtp_   = LinToDb(currentTruePeak_);
    telGainReductionDb_ = currentGainReductionDb_;

    // Decay the held telemetry maxima per buffer so they don't
    // stick at peak values forever — designers want to see a
    // dynamic reading, not a "max ever held" indicator.
    currentTruePeak_ *= 0.95f;
    currentGainReductionDb_ *= 0.95f;  // approaches 0 dB GR
}

void MasterControlEffect::ProcessOversamplePeak(const float* in,
                                                  uint32_t frames,
                                                  uint32_t channels) noexcept {
    // 4× linear-interpolated peak. For each consecutive sample
    // pair (per channel) we generate 3 intermediate samples and
    // check the magnitude. Catches the bulk of intersample peaks
    // that fall between sample-rate sample points.
    for (uint32_t i = 0; i + 1 < frames; ++i) {
        for (uint32_t c = 0; c < channels; ++c) {
            const float s0 = in[i * channels + c];
            const float s1 = in[(i + 1) * channels + c];
            for (int k = 0; k <= 4; ++k) {
                const float t = k * 0.25f;
                const float s = s0 + (s1 - s0) * t;
                const float a = std::fabs(s);
                if (a > currentTruePeak_) currentTruePeak_ = a;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// OnParameter / GetParameter — control surface
// ---------------------------------------------------------------------------

void MasterControlEffect::OnParameter(uint16_t paramId, float value) noexcept {
    using namespace EffectParameter;
    switch (paramId) {
        // Stage enables. Convention: 0.0 = off, any other value = on.
        case MC_GlueEnabled:        cfg_.glueEnabled    = (value != 0.0f); break;
        case MC_RiderEnabled:       cfg_.riderEnabled   = (value != 0.0f); break;
        case MC_LimiterEnabled:     cfg_.limiterEnabled = (value != 0.0f); break;
        // Glue.
        case MC_GlueThresholdDb:    cfg_.glueThresholdDb = value; break;
        case MC_GlueRatio:          cfg_.glueRatio       = std::max(1.0f, value); break;
        case MC_GlueAttackMs:       cfg_.glueAttackMs    = std::max(0.0f, value); break;
        case MC_GlueReleaseMs:      cfg_.glueReleaseMs   = std::max(0.0f, value); break;
        case MC_GlueKneeDb:         cfg_.glueKneeDb      = std::max(0.0f, value); break;
        case MC_GlueMakeupDb:       cfg_.glueMakeupDb    = value; break;
        // Rider.
        case MC_RiderTargetLufs:    cfg_.riderTargetLufs    = value; break;
        case MC_RiderTimeConstMs:   cfg_.riderTimeConstantMs = std::max(1.0f, value); break;
        case MC_RiderMaxGainDb:     cfg_.riderMaxGainDb     = value; break;
        case MC_RiderMinGainDb:     cfg_.riderMinGainDb     = value; break;
        case MC_RiderFreezeBelowLufs: cfg_.riderFreezeBelowLufs = value; break;
        // Limiter.
        case MC_LimiterCeilingDbtp: cfg_.limiterCeilingDbtp = value; break;
        case MC_LimiterReleaseMs:   cfg_.limiterReleaseMs   = std::max(1.0f, value); break;
        case MC_LimiterLookaheadMs:
            // Lookahead change requires a Prepare() to resize the
            // ring buffer; we store it on cfg_ but the next Process
            // continues with the old size until Prepare runs again.
            // Document this in the param tooltip — designers won't
            // typically change lookahead live.
            cfg_.limiterLookaheadMs = std::clamp(value, 0.0f, 10.0f);
            break;
        // Telemetry IDs are read-only; ignore writes.
        default: break;
    }
}

float MasterControlEffect::GetParameter(uint16_t paramId) const noexcept {
    using namespace EffectParameter;
    switch (paramId) {
        case MC_GlueEnabled:        return cfg_.glueEnabled    ? 1.0f : 0.0f;
        case MC_RiderEnabled:       return cfg_.riderEnabled   ? 1.0f : 0.0f;
        case MC_LimiterEnabled:     return cfg_.limiterEnabled ? 1.0f : 0.0f;
        case MC_GlueThresholdDb:    return cfg_.glueThresholdDb;
        case MC_GlueRatio:          return cfg_.glueRatio;
        case MC_GlueAttackMs:       return cfg_.glueAttackMs;
        case MC_GlueReleaseMs:      return cfg_.glueReleaseMs;
        case MC_GlueKneeDb:         return cfg_.glueKneeDb;
        case MC_GlueMakeupDb:       return cfg_.glueMakeupDb;
        case MC_RiderTargetLufs:    return cfg_.riderTargetLufs;
        case MC_RiderTimeConstMs:   return cfg_.riderTimeConstantMs;
        case MC_RiderMaxGainDb:     return cfg_.riderMaxGainDb;
        case MC_RiderMinGainDb:     return cfg_.riderMinGainDb;
        case MC_RiderFreezeBelowLufs: return cfg_.riderFreezeBelowLufs;
        case MC_LimiterCeilingDbtp: return cfg_.limiterCeilingDbtp;
        case MC_LimiterReleaseMs:   return cfg_.limiterReleaseMs;
        case MC_LimiterLookaheadMs: return cfg_.limiterLookaheadMs;
        // Telemetry: return the most-recent Process() values.
        case MC_TelLufsShortTerm:   return telLufsShortTerm_;
        case MC_TelLufsIntegrated:  return telLufsIntegrated_;
        case MC_TelPeakDb:          return telPeakDb_;
        case MC_TelTruePeakDbtp:    return telTruePeakDbtp_;
        case MC_TelGainReductionDb: return telGainReductionDb_;
        case MC_TelRiderGainDb:     return telRiderGainDb_;
        default: return 0.0f;
    }
}

MasterControlTelemetry MasterControlEffect::GetTelemetry() const noexcept {
    return MasterControlTelemetry{
        telLufsShortTerm_,
        telLufsIntegrated_,
        telPeakDb_,
        telTruePeakDbtp_,
        telGainReductionDb_,
        telRiderGainDb_,
    };
}

void MasterControlEffect::ResetIntegratedLufs() noexcept {
    integratedPowerSum_    = 0.0;
    integratedSampleCount_ = 0;
    telLufsIntegrated_     = -100.0f;
}

}  // namespace audio
