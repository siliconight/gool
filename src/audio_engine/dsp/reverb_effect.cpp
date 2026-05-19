// audio_engine/dsp/reverb_effect.cpp
//
// Dattorro plate reverb implementation. See reverb_effect.h for design
// notes and parameter semantics, and docs/audio_design/reverb_dattorro.md
// for the full design context.

#include "audio_engine/dsp/reverb_effect.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

// Dattorro's published delay lengths in samples at his original 29761 Hz
// reference rate. Scaled to the runtime sample rate via ScaleDelay below.
//
// Layout: input diffuser is 4 allpasses; tank has 4 delays (delay1..4) and
// 2 in-tank allpasses (ap2 in half A, ap4 in half B).
constexpr float kRefSampleRate = 29761.0f;

constexpr uint32_t kDiffuserLengthsRef[4] = { 142u, 107u, 379u, 277u };
constexpr float    kDiffuserBaseGainAB    = 0.75f;     // AP1, AP2 (pre-diffusion scaling)
constexpr float    kDiffuserBaseGainCD    = 0.625f;    // AP3, AP4

// Modulated allpasses sit at the entrance of each tank half.
constexpr uint32_t kModApBaseRefA = 672u;
constexpr uint32_t kModApBaseRefB = 908u;
constexpr float    kModApGain     = -0.7f;
constexpr float    kModDepthSamples = 8.0f;          // applied at any SR
constexpr float    kModLfoHzA     = 0.5f;
constexpr float    kModLfoHzB     = 0.3f;            // slightly offset to decorrelate

// Tank delays.
constexpr uint32_t kDelay1Ref = 4453u;
constexpr uint32_t kDelay2Ref = 3720u;
constexpr uint32_t kDelay3Ref = 4217u;
constexpr uint32_t kDelay4Ref = 3163u;

// In-tank Schroeder allpasses (the non-modulated ones).
constexpr uint32_t kAp2Ref   = 1800u;
constexpr uint32_t kAp4Ref   = 2656u;
constexpr float    kInTankApGain = 0.5f;

// Output tap positions (samples back from current write head, at the
// reference 29761 Hz rate). These are Dattorro's published "magic
// numbers" — the specific positions that give the algorithm its
// characteristic spaciousness and stereo image. Scaled like delays.
//
// Convention here: tapL[i] / tapR[i] each have 7 entries paired with
// the source delay/ap they read from (encoded in the Process tap-sum
// expression directly, since each tap reads from a specific node).
constexpr uint32_t kTapL_delay1_a = 266u;
constexpr uint32_t kTapL_delay1_b = 2974u;
constexpr uint32_t kTapL_ap2      = 1913u;
constexpr uint32_t kTapL_delay2   = 1996u;
constexpr uint32_t kTapL_delay3   = 1990u;
constexpr uint32_t kTapL_ap4      = 187u;
constexpr uint32_t kTapL_delay4   = 1066u;

constexpr uint32_t kTapR_delay3_a = 353u;
constexpr uint32_t kTapR_delay3_b = 3627u;
constexpr uint32_t kTapR_ap4      = 1228u;
constexpr uint32_t kTapR_delay4   = 2673u;
constexpr uint32_t kTapR_delay1   = 2111u;
constexpr uint32_t kTapR_ap2      = 335u;
constexpr uint32_t kTapR_delay2   = 121u;

// Maximum predelay we ever allocate buffer space for. 200 ms is generous;
// most useful predelay settings are 5..80 ms.
constexpr float kMaxPredelayMs = 200.0f;

// Default predelay floor in samples. Even at predelay_ms = 0 we keep a
// 1-sample minimum so the predelay path always has somewhere to read from.
constexpr uint32_t kMinPredelaySamples = 1u;

// Scale a reference-rate sample count to the runtime sample rate, rounded
// to nearest.
inline uint32_t ScaleDelay(uint32_t refLen, uint32_t sampleRate) noexcept {
    const float ratio = static_cast<float>(sampleRate) / kRefSampleRate;
    return static_cast<uint32_t>(static_cast<float>(refLen) * ratio + 0.5f);
}

inline float DbToLinear(float db) noexcept {
    return std::pow(10.0f, db / 20.0f);
}

// One full turn for the LFO sine. std::sin takes radians.
constexpr float kTwoPi = 6.28318530717958647692f;

} // anonymous namespace

// ---- ModulatedAllpass::Step ------------------------------------------------
//
// Reads from a moving tap position centered on baseDelay, offset by
// modDepth * sin(2π·phase). Linear interpolation between adjacent samples
// avoids the audible clicks an integer-sample-jump implementation would
// produce. After computing the allpass output, writes the standard
// Schroeder combination to the line.

float ReverbEffect::ModulatedAllpass::Step(float x) noexcept {
    const float modOffset = modDepth * std::sin(kTwoPi * lfoPhase);
    const float readPos = baseDelay + modOffset;
    const float d = line.ReadFractional(readPos);
    const float y = -gain * x + d;
    line.Write(x + gain * d);
    line.Advance();
    lfoPhase += lfoIncr;
    if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
    return y;
}

// ---- DampingShelf::Configure -----------------------------------------------
//
// Maps the user-facing [0..1] damping parameters to internal coefficients:
//
//   hfDamping = 0  →  hfCoef = 1.0  (one-pole LP fully open; no HF damping)
//   hfDamping = 1  →  hfCoef ≈ 0.05 (LP cutoff drops near DC; heavy HF damping)
//
// The LF tracking filter's cutoff is fixed at ~120 Hz regardless of HF
// damping. lfDampGain is 0..1; full subtraction (1.0) attenuates the LF
// energy without notching it out completely (which would sound unnatural).

void ReverbEffect::DampingShelf::Configure(float lfDamping, float hfDamping,
                                             uint32_t sampleRate) noexcept {
    lfDamping = std::clamp(lfDamping, 0.0f, 1.0f);
    hfDamping = std::clamp(hfDamping, 0.0f, 1.0f);

    // HF damping: smooth from full pass (1.0) down to heavy damp (0.05).
    // At hfCoef = 1.0, the one-pole acts as a wire; at 0.05 it's heavily
    // attenuating everything above ~hundreds of Hz.
    hfCoef_ = 1.0f - hfDamping * 0.95f;

    // LF tracking cutoff fixed at ~120 Hz. Small-angle approximation:
    //   a ≈ 2π fc / SR
    // Bounded to avoid degenerate values at extreme sample rates.
    constexpr float kLfTrackHz = 120.0f;
    lfCoef_ = std::clamp(kTwoPi * kLfTrackHz / static_cast<float>(sampleRate),
                         0.001f, 0.3f);

    lfDamp_ = lfDamping;
}

// ---- Constructor + derived recompute --------------------------------------

ReverbEffect::ReverbEffect(float predelayMs, float decay,
                             float lfDamping, float hfDamping,
                             float diffusion, float wetGainDb)
    : predelayMs_(std::clamp(predelayMs, 0.0f, kMaxPredelayMs)),
      decay_(std::clamp(decay, 0.0f, 1.0f)),
      lfDamping_(std::clamp(lfDamping, 0.0f, 1.0f)),
      hfDamping_(std::clamp(hfDamping, 0.0f, 1.0f)),
      diffusion_(std::clamp(diffusion, 0.0f, 1.0f)),
      wetGainDb_(wetGainDb)
{
    // Derived values are computed in Prepare once we know the sample rate.
    // No-op here; Prepare will be called before Process.
}

void ReverbEffect::RecomputeWetGain() noexcept {
    wetLin_ = DbToLinear(wetGainDb_);
}

// Decay parameter maps to internal tank-feedback gain via:
//   feedback = 0.4 + decay * 0.52   ⇒  [0.40, 0.92]
//
// Bounds chosen empirically: 0.4 gives a perceptible-but-short tail at
// decay=0, 0.92 gives a long stable tail at decay=1. Above 0.95 the
// numerical headroom for tank state shrinks enough to risk instability
// under heavy input, so we cap there.
void ReverbEffect::RecomputeDecayFeedback() noexcept {
    tankFeedback_ = 0.4f + decay_ * 0.52f;
}

// Diffusion scales the input-diffuser allpass gains by diffusion_,
// preserving their relative magnitudes. At diffusion=0 the diffuser is
// a wire; at diffusion=1 it uses Dattorro's published gains.
void ReverbEffect::RecomputeDiffusion() noexcept {
    diffuserGainAB_ = kDiffuserBaseGainAB * diffusion_;
    diffuserGainCD_ = kDiffuserBaseGainCD * diffusion_;
    inputDiffuser_[0].gain = diffuserGainAB_;
    inputDiffuser_[1].gain = diffuserGainAB_;
    inputDiffuser_[2].gain = diffuserGainCD_;
    inputDiffuser_[3].gain = diffuserGainCD_;
}

void ReverbEffect::RecomputePredelay() noexcept {
    const float samples = predelayMs_ * 0.001f * static_cast<float>(sampleRate_);
    predelaySamples_ = std::max(kMinPredelaySamples,
                                  static_cast<uint32_t>(samples + 0.5f));
    // The predelay buffer is sized for kMaxPredelayMs in Prepare; here we
    // just clamp the active tap. ReadTap will use predelaySamples_ as the
    // offset-back when reading.
}

void ReverbEffect::RecomputeDamping() noexcept {
    for (auto& half : tank_) {
        half.damping.Configure(lfDamping_, hfDamping_, sampleRate_);
    }
}

// ---- Prepare ---------------------------------------------------------------

void ReverbEffect::Prepare(uint32_t sampleRate, uint32_t channels) {
    sampleRate_ = (sampleRate == 0) ? 48000u : sampleRate;
    channels_   = (channels  == 0) ? 1u     : channels;

    // Predelay buffer sized for the maximum we'll ever ask for.
    const uint32_t predelayMax = static_cast<uint32_t>(
        kMaxPredelayMs * 0.001f * static_cast<float>(sampleRate_) + 0.5f);
    predelay_.buf.assign(std::max(predelayMax, 1u), 0.0f);
    predelay_.Reset();

    // Input diffuser allpasses.
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t len = ScaleDelay(kDiffuserLengthsRef[i], sampleRate_);
        inputDiffuser_[i].line.buf.assign(std::max(len, 1u), 0.0f);
        inputDiffuser_[i].Reset();
    }

    // Tank half A.
    {
        TankHalf& A = tank_[0];
        A.modAP.baseDelay = static_cast<float>(
            ScaleDelay(kModApBaseRefA, sampleRate_));
        const uint32_t modBufLen =
            static_cast<uint32_t>(A.modAP.baseDelay)
            + static_cast<uint32_t>(kModDepthSamples) * 2u
            + 4u;
        A.modAP.line.buf.assign(modBufLen, 0.0f);
        A.modAP.gain = kModApGain;
        A.modAP.modDepth = kModDepthSamples;
        A.modAP.lfoIncr  = kModLfoHzA / static_cast<float>(sampleRate_);
        A.modAP.lfoPhase = 0.0f;

        A.delay1.buf.assign(
            std::max(ScaleDelay(kDelay1Ref, sampleRate_), 1u), 0.0f);
        A.delay1.Reset();

        A.ap.line.buf.assign(
            std::max(ScaleDelay(kAp2Ref, sampleRate_), 1u), 0.0f);
        A.ap.gain = kInTankApGain;

        A.delay2.buf.assign(
            std::max(ScaleDelay(kDelay2Ref, sampleRate_), 1u), 0.0f);
        A.delay2.Reset();
    }

    // Tank half B.
    {
        TankHalf& B = tank_[1];
        B.modAP.baseDelay = static_cast<float>(
            ScaleDelay(kModApBaseRefB, sampleRate_));
        const uint32_t modBufLen =
            static_cast<uint32_t>(B.modAP.baseDelay)
            + static_cast<uint32_t>(kModDepthSamples) * 2u
            + 4u;
        B.modAP.line.buf.assign(modBufLen, 0.0f);
        B.modAP.gain = kModApGain;
        B.modAP.modDepth = kModDepthSamples;
        B.modAP.lfoIncr  = kModLfoHzB / static_cast<float>(sampleRate_);
        B.modAP.lfoPhase = 0.25f;   // start 90° offset from half A

        B.delay1.buf.assign(
            std::max(ScaleDelay(kDelay3Ref, sampleRate_), 1u), 0.0f);
        B.delay1.Reset();

        B.ap.line.buf.assign(
            std::max(ScaleDelay(kAp4Ref, sampleRate_), 1u), 0.0f);
        B.ap.gain = kInTankApGain;

        B.delay2.buf.assign(
            std::max(ScaleDelay(kDelay4Ref, sampleRate_), 1u), 0.0f);
        B.delay2.Reset();
    }

    crossFromA_ = 0.0f;
    crossFromB_ = 0.0f;

    RecomputeWetGain();
    RecomputeDecayFeedback();
    RecomputeDiffusion();
    RecomputePredelay();
    RecomputeDamping();
}

// ---- Process ---------------------------------------------------------------
//
// Per-sample signal flow:
//
//   1.  Read predelay tap; write input into predelay; advance.
//   2.  Run through 4 input-diffuser allpasses → tankIn.
//   3.  Tank half A: input = tankIn + crossFromB
//       runs modAP → delay1.Write/Read/Advance → damping → ap → delay2.WRA
//   4.  Tank half B: input = tankIn + crossFromA (symmetric)
//   5.  crossFromA = halfA_output × tankFeedback (for NEXT sample)
//       crossFromB = halfB_output × tankFeedback
//   6.  Sum 7 weighted output taps for L and 7 for R.
//   7.  Write outputs (× wetLin) to the buffer.
//
// The reverb is wet-only — it overwrites the bus's accumulated sample.
// The dry path lives on a different bus, summed at master.

void ReverbEffect::Process(float* output, uint32_t frames, uint32_t channels,
                             const float* /*sidechain*/,
                             uint32_t /*sidechainCh*/) noexcept {
    if (channels == 0 || frames == 0) return;

    // Empirical input scaling. Freeverb used 0.015; Dattorro's algorithm
    // has higher internal gain, so a lower input scaling keeps the wet
    // level sensible at wet_gain_db = 0 for typical material.
    constexpr float kInputGain = 0.5f;

    for (uint32_t f = 0; f < frames; ++f) {
        // Mix to mono for the reverb input.
        const float inL = output[f * channels + 0];
        const float inR = (channels >= 2) ? output[f * channels + 1] : inL;
        const float monoIn = (inL + inR) * 0.5f * kInputGain;

        // Predelay: read the sample written `predelaySamples_` ago, then
        // write the new sample at the current head.
        const float preOut = predelay_.ReadTap(predelaySamples_);
        predelay_.Write(monoIn);
        predelay_.Advance();

        // Input diffuser: 4 allpasses in series.
        float diffused = preOut;
        diffused = inputDiffuser_[0].Step(diffused);
        diffused = inputDiffuser_[1].Step(diffused);
        diffused = inputDiffuser_[2].Step(diffused);
        diffused = inputDiffuser_[3].Step(diffused);
        const float tankIn = diffused;

        // Tank half A.
        TankHalf& A = tank_[0];
        const float aIn  = tankIn + crossFromB_;
        const float aMod = A.modAP.Step(aIn);
        A.delay1.Write(aMod);
        const float aD1 = A.delay1.Read();
        A.delay1.Advance();
        const float aDamped = A.damping.Step(aD1);
        const float aApOut  = A.ap.Step(aDamped);
        A.delay2.Write(aApOut);
        const float aOut = A.delay2.Read();
        A.delay2.Advance();

        // Tank half B (symmetric).
        TankHalf& B = tank_[1];
        const float bIn  = tankIn + crossFromA_;
        const float bMod = B.modAP.Step(bIn);
        B.delay1.Write(bMod);
        const float bD1 = B.delay1.Read();
        B.delay1.Advance();
        const float bDamped = B.damping.Step(bD1);
        const float bApOut  = B.ap.Step(bDamped);
        B.delay2.Write(bApOut);
        const float bOut = B.delay2.Read();
        B.delay2.Advance();

        // Stash cross-feedback for next sample.
        crossFromA_ = aOut * tankFeedback_;
        crossFromB_ = bOut * tankFeedback_;

        // Output taps. Names refer to the topology:
        //   A.delay1 ↔ Dattorro's delay1     A.ap ↔ ap2     A.delay2 ↔ delay2
        //   B.delay1 ↔ Dattorro's delay3     B.ap ↔ ap4     B.delay2 ↔ delay4
        // Tap positions are scaled to the runtime sample rate (using the
        // helper inline rather than caching, since the cost is negligible
        // and the constants get const-folded by the compiler).
        const float yL =
              A.delay1.ReadTap(ScaleDelay(kTapL_delay1_a, sampleRate_))
            + A.delay1.ReadTap(ScaleDelay(kTapL_delay1_b, sampleRate_))
            - A.ap    .line.ReadTap(ScaleDelay(kTapL_ap2,    sampleRate_))
            + A.delay2.ReadTap(ScaleDelay(kTapL_delay2,   sampleRate_))
            - B.delay1.ReadTap(ScaleDelay(kTapL_delay3,   sampleRate_))
            - B.ap    .line.ReadTap(ScaleDelay(kTapL_ap4,    sampleRate_))
            - B.delay2.ReadTap(ScaleDelay(kTapL_delay4,   sampleRate_));

        const float yR =
              B.delay1.ReadTap(ScaleDelay(kTapR_delay3_a, sampleRate_))
            + B.delay1.ReadTap(ScaleDelay(kTapR_delay3_b, sampleRate_))
            - B.ap    .line.ReadTap(ScaleDelay(kTapR_ap4,    sampleRate_))
            + B.delay2.ReadTap(ScaleDelay(kTapR_delay4,   sampleRate_))
            - A.delay1.ReadTap(ScaleDelay(kTapR_delay1,   sampleRate_))
            - A.ap    .line.ReadTap(ScaleDelay(kTapR_ap2,    sampleRate_))
            - A.delay2.ReadTap(ScaleDelay(kTapR_delay2,   sampleRate_));

        // 7 taps each, summed with mixed signs ⇒ roughly unity gain at
        // moderate decay. wetLin_ is the user trim on top.
        output[f * channels + 0] = yL * wetLin_;
        if (channels >= 2) {
            output[f * channels + 1] = yR * wetLin_;
        }
    }
}

// ---- OnParameter -----------------------------------------------------------
//
// Soft migration: the v0.28.x parameter IDs Reverb_RoomSize and
// Reverb_Damping map to the same numeric values as the new Reverb_Decay
// and Reverb_HfDamping. The switch labels here use the new names; the
// old enum constants in bus.h are aliases that compile to the same
// case values, so no explicit deprecation branch is needed.

void ReverbEffect::OnParameter(uint16_t paramId, float value) noexcept {
    switch (paramId) {
        case EffectParameter::Reverb_Decay:
            decay_ = std::clamp(value, 0.0f, 1.0f);
            RecomputeDecayFeedback();
            break;
        case EffectParameter::Reverb_HfDamping:
            hfDamping_ = std::clamp(value, 0.0f, 1.0f);
            RecomputeDamping();
            break;
        case EffectParameter::Reverb_WetGainDb:
            wetGainDb_ = value;
            RecomputeWetGain();
            break;
        case EffectParameter::Reverb_PredelayMs:
            predelayMs_ = std::clamp(value, 0.0f, kMaxPredelayMs);
            RecomputePredelay();
            break;
        case EffectParameter::Reverb_LfDamping:
            lfDamping_ = std::clamp(value, 0.0f, 1.0f);
            RecomputeDamping();
            break;
        case EffectParameter::Reverb_Diffusion:
            diffusion_ = std::clamp(value, 0.0f, 1.0f);
            RecomputeDiffusion();
            break;
        default:
            break;
    }
}

float ReverbEffect::GetParameter(uint16_t paramId) const noexcept {
    switch (paramId) {
        case EffectParameter::Reverb_Decay:        return decay_;
        case EffectParameter::Reverb_HfDamping:    return hfDamping_;
        case EffectParameter::Reverb_WetGainDb:    return wetGainDb_;
        case EffectParameter::Reverb_PredelayMs:   return predelayMs_;
        case EffectParameter::Reverb_LfDamping:    return lfDamping_;
        case EffectParameter::Reverb_Diffusion:    return diffusion_;
        default:                                    return 0.0f;
    }
}

} // namespace audio
