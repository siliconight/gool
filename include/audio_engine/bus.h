// audio_engine/bus.h
//
// Bus graph and DSP effect configuration. The host defines a tree of named
// buses at Initialize() time; buses route signal up the tree to the master
// bus. Effects live in a per-bus chain; an effect on bus A can declare a
// sidechain reference to bus B, which constrains the render thread to
// process B before A within the same callback.
//
// Ducking is the canonical use case: place a Compressor on Music with
// sidechainBus = LocalGun. When the player fires their weapon, energy on
// LocalGun drives the compressor's envelope and ducks Music.
//
// All bus and effect configuration is consumed once at Initialize() and is
// immutable after that. Parameter values can be updated at runtime through
// AudioRuntime::SetBusGainDb() and AudioRuntime::SetEffectParameter().

#ifndef AUDIO_ENGINE_BUS_H
#define AUDIO_ENGINE_BUS_H

#include <cstdint>
#include "audio_engine/types.h"

namespace audio {

// ---- Identifiers ----------------------------------------------------------

using BusId = uint16_t;

constexpr BusId kBusMaster      = 0;     // always exists; sink for everything
// Conventional ID for the reverb send target. The mixer routes a fraction of
// each voice's signal here when SpatialParams.reverbSend > 0. The host opts
// in by adding a bus with this id (typically with a single ReverbEffect on
// it). If no bus has this id, sends are silently no-op.
constexpr BusId kBusReverb      = 1;
constexpr BusId kInvalidBusId   = 0xFFFF;

constexpr uint32_t kMaxBuses              = 32;
constexpr uint32_t kMaxEffectsPerBus      = 8;
constexpr uint32_t kMaxEffectsTotal       = kMaxBuses * kMaxEffectsPerBus;

// ---- Effect configuration -------------------------------------------------

enum class EffectKind : uint8_t {
    None        = 0,
    Gain,           // linear gain (smoothed internally on parameter change)
    BiquadFilter,   // LPF / HPF / BPF
    Compressor,     // envelope-driven gain reduction; supports sidechain
    Reverb,         // Schroeder/Freeverb-style algorithmic reverb
    Saturation,     // v0.40.0: 4-mode multi-shape waveshaper (Tanh, Tube, Tape, Diode)
    MasterControl,  // v0.63.0: glue compressor + LUFS gain rider + true-peak limiter
};

enum class BiquadType : uint8_t {
    LowPass   = 0,
    HighPass,
    BandPass,
    // Tone-shaping filters. cutoff is the corner / transition / center
    // frequency; Q controls bandwidth (peak) or transition steepness
    // (shelf); gainDb is the boost (positive) or cut (negative) at
    // the relevant frequency band. These types ignore Q for shelves
    // and use it as bandwidth for peak.
    LowShelf,        // boost/cut frequencies below cutoff
    HighShelf,       // boost/cut frequencies above cutoff
    Peak,            // boost/cut a band centered at cutoff
};

// Tagged-struct effect descriptor. The `kind` field selects which named
// parameters are read; unused parameters are ignored. Designed to be a flat
// POD so AudioConfig can carry an array of these by value.
struct EffectConfig {
    EffectKind kind = EffectKind::None;

    // Gain
    float gainDb = 0.0f;

    // Biquad filter
    BiquadType biquadType = BiquadType::LowPass;
    float      biquadCutoffHz = 20000.0f;
    float      biquadQ        = 0.707f;     // butterworth
    float      biquadGainDb   = 0.0f;       // honored by LowShelf/HighShelf/Peak only

    // Compressor
    float compressorThresholdDb  = -20.0f;
    float compressorRatio        = 4.0f;
    float compressorAttackMs     = 10.0f;
    float compressorReleaseMs    = 200.0f;
    float compressorMakeupDb     = 0.0f;
    BusId compressorSidechainBus = kInvalidBusId;  // self-sidechain when invalid

    // ---- Tier-A (v0.8) compressor parameters ----
    // Soft-knee width in dB. 0 = hard knee (default, matches legacy
    // behavior). Typical values 3–12 dB; the gain reduction curve
    // transitions quadratically from no-compression to full-compression
    // across this width centered on the threshold.
    float compressorKneeWidthDb  = 0.0f;
    // Parallel-compression dry/wet mix. 1.0 = fully wet (default,
    // matches legacy behavior). 0.0 = bypass. Useful for "New York"-
    // style drum thickening, gunshots, etc.
    float compressorMixRatio     = 1.0f;
    // Maximum gain reduction in dB. Caps the compressor's authority
    // so a runaway transient can't fully duck the signal. Default
    // 60 dB = effectively unlimited; useful values 6–18 dB.
    float compressorMaxReductionDb = 60.0f;
    // Sidechain high-pass filter cutoff in Hz. 0 = bypass (default).
    // Filters the detection signal so low-frequency content (kicks,
    // explosions) doesn't over-trigger the compressor — modern game
    // audio table stakes for music ducking under VO.
    float compressorSidechainHpfHz = 0.0f;
    // Hold time in ms. After the detection envelope drops below the
    // threshold, the release stage is delayed by this duration. 0 =
    // no hold (default). Stabilizes dialogue ducking and avoids
    // chatter when the trigger source has gaps.
    float compressorHoldMs       = 0.0f;
    // Detection mode. Peak (default) = instantaneous |sample|; reacts
    // hard to transients. Rms = sqrt(mean(square)); smoother, more
    // musical, common for music/dialogue ducking.
    enum class CompressorDetectionMode : uint8_t {
        Peak = 0,
        Rms  = 1,
    };
    CompressorDetectionMode compressorDetectionMode =
        CompressorDetectionMode::Peak;

    // Reverb (Schroeder/Freeverb-derived)
    //   roomSize  0..1; feedback gain → tail length
    //   damping   0..1; high-frequency rolloff in feedback path
    //   wetGainDb    ; gain applied to the wet output of this effect.
    //                   The reverb bus sums into master at its configured
    //                   outputGainDb; wetGainDb is the effect-internal mix
    //                   level (default 0 = unity).
    // Reverb (Dattorro plate, v0.29.0+).
    //
    // The 0..1 parameters all default to "moderate room" character at
    // sensible levels. Predelay defaults to 30 ms — a reasonable
    // "medium-small room" cue without being so short that the effect
    // sounds like a chorus. Diffusion default 0.625 matches Dattorro's
    // published baseline; lower values produce a more "echo-y" early
    // signal, higher values are smoother.
    //
    // Soft migration from the v0.28.x Freeverb impl: old config files
    // using `room_size` and `damping` JSON keys are routed by the
    // loader into reverbDecay and reverbHfDamping respectively
    // (the semantic mapping is 1:1). See bus_config_loader.cpp.
    float reverbPredelayMs = 30.0f;   // 0..200 ms
    float reverbDecay      = 0.5f;    // 0..1 — tank feedback length
    float reverbLfDamping  = 0.0f;    // 0..1 — low-frequency absorption
    float reverbHfDamping  = 0.3f;    // 0..1 — high-frequency absorption
    float reverbDiffusion  = 0.625f;  // 0..1 — input-diffuser gain scale
    // Dry/wet mix levels in dB. v0.29.5 added reverbDryGainDb so the
    // reverb can sit on an insert position (signal + wet together) as
    // well as on a send/return bus (dry muted, wet only). Default 0 dB
    // dry = unity passthrough of the input. For classic send/return
    // routing, set reverbDryGainDb to a very negative value (e.g.
    // -60 dB) so the reverb bus emits wet only and the user mixes
    // the dry signal in via a separate (parallel) bus path.
    float reverbDryGainDb  = 0.0f;    // pre-effect dry passthrough, dB
    float reverbWetGainDb  = 0.0f;    // post-effect wet trim, dB

    // Saturation (tanh waveshaper)
    //   drive       ; pre-shaper input gain. > 1 generates harmonics.
    //                  1.0 = no harmonics. Typical 1.5–4.0.
    //   mix         ; dry/wet blend. 0 = bypass (default — installing
    //                  the effect on a bus is a no-op until you turn
    //                  this up). Subtle glue lives at 0.10–0.30.
    //   outputGain  ; post-shaper trim. Compensates for loudness
    //                  change drive introduces; rule of thumb
    //                  ~ 1.0/sqrt(drive).
    //   bias        ; DC offset before shaping (DC-corrected at
    //                  output). 0 = symmetric (odd harmonics only,
    //                  "tape"-ish). Non-zero introduces even
    //                  harmonics ("tube"/"warmth"). Typical 0.05–0.20.
    //   mode        ; v0.40.0 — shape function selector. See
    //                  SaturationMode below. Default 0 (Tanh), which
    //                  matches pre-v0.40.0 behavior bit-for-bit at
    //                  the equivalent normalized drive.
    float saturationDrive       = 1.0f;
    float saturationMix         = 0.0f;
    float saturationOutputGain  = 1.0f;
    float saturationBias        = 0.0f;
    uint8_t saturationMode      = 0;       // 0=Tanh, 1=Tube, 2=Tape, 3=Diode (v0.40.0)
    // v0.59.0: Phase 4 tone tilt. -1..+1, default 0 (filter bypassed).
    // Negative values pre-cut highs (lows drive shaper harder → darker);
    // positive values pre-boost highs (HF drives shaper harder →
    // brighter). De-emphasis post-shaper restores tonal balance on
    // dry-equivalent material so the net is character change, not
    // EQ change. See docs/audio_design/saturation_v2.md §9.
    float saturationTone        = 0.0f;

    // ---- MasterControl (v0.63.0) ----
    // See audio_engine/dsp/master_control.h (MasterControlConfig) for
    // semantics. Field names match. Defaults here mirror the
    // "Standard FPS" preset: glue + rider + limiter all on,
    // -16 LUFS target, -1 dBTP ceiling.
    bool  mcGlueEnabled              = true;
    bool  mcRiderEnabled             = true;
    bool  mcLimiterEnabled           = true;
    float mcGlueThresholdDb          = -12.0f;
    float mcGlueRatio                = 2.0f;
    float mcGlueAttackMs             = 10.0f;
    float mcGlueReleaseMs            = 250.0f;
    float mcGlueKneeDb               = 6.0f;
    float mcGlueMakeupDb             = 0.0f;
    float mcRiderTargetLufs          = -16.0f;
    float mcRiderTimeConstantMs      = 3000.0f;
    float mcRiderMaxGainDb           = 6.0f;
    float mcRiderMinGainDb           = -6.0f;
    float mcRiderFreezeBelowLufs     = -6.0f;
    float mcLimiterCeilingDbtp       = -1.0f;
    float mcLimiterReleaseMs         = 50.0f;
    float mcLimiterLookaheadMs       = 5.0f;
};

// v0.40.0: shape mode selector for the multi-character saturation
// engine. Each mode has a different acoustic character (driven by a
// different memoryless nonlinearity) and a different useful drive
// range; drive itself is normalized 0..1 in the v0.40.0+ API and
// mapped to the per-mode useful range internally. See
// docs/audio_design/saturation_v2.md §6 for mode-by-mode rationale.
enum class SaturationMode : uint8_t {
    Tanh  = 0,   // tanh(x), drive 1..4, symmetric, odd-harmonic-dominant. Default.
    Tube  = 1,   // asinh(x)/asinh(1), drive 1..3, gentle shoulder, unbounded.
    Tape  = 2,   // soft-quadratic Zölzer, drive 1..3, bounded ±1, parabolic shoulder.
    Diode = 3,   // x - x³/3 clamped at ±2/3, drive 1..6, sharp shoulder, gunshot bite.
};

// Parameter IDs accepted by SetEffectParameter. Not every effect honors
// every ID; mismatched IDs are ignored on the render thread.
namespace EffectParameter {
    constexpr uint16_t Gain_GainDb              = 1;
    constexpr uint16_t Biquad_CutoffHz          = 2;
    constexpr uint16_t Biquad_Q                 = 3;
    constexpr uint16_t Compressor_ThresholdDb   = 4;
    constexpr uint16_t Compressor_Ratio         = 5;
    constexpr uint16_t Compressor_AttackMs      = 6;
    constexpr uint16_t Compressor_ReleaseMs     = 7;
    constexpr uint16_t Compressor_MakeupDb      = 8;
    // Reverb. v0.29.0 renamed RoomSize → Decay and Damping → HfDamping
    // when the Freeverb impl was replaced with a Dattorro plate. Old
    // names are kept as deprecated aliases at the same numeric IDs so
    // external code and config files keep working unchanged. The
    // semantic mapping is 1:1 (decay ↔ feedback length, hf_damping ↔
    // tail brightness), so the behavior shift is minimal.
    constexpr uint16_t Reverb_Decay             = 9;
    constexpr uint16_t Reverb_HfDamping         = 10;
    constexpr uint16_t Reverb_WetGainDb         = 11;
    // Deprecated aliases (same IDs as above; kept for back-compat).
    constexpr uint16_t Reverb_RoomSize          = Reverb_Decay;
    constexpr uint16_t Reverb_Damping           = Reverb_HfDamping;
    constexpr uint16_t Biquad_GainDb            = 12;
    // Tier-A (v0.8) compressor parameters. SetEffectParameter accepts
    // floats, so the detection-mode IDs are encoded as 0.0f = Peak,
    // 1.0f = Rms (any other value clamps to Peak for safety).
    constexpr uint16_t Compressor_KneeWidthDb   = 13;
    constexpr uint16_t Compressor_MixRatio      = 14;
    constexpr uint16_t Compressor_MaxReductionDb = 15;
    constexpr uint16_t Compressor_SidechainHpfHz = 16;
    constexpr uint16_t Compressor_HoldMs        = 17;
    constexpr uint16_t Compressor_DetectionMode = 18;
    // Saturation (v0.9). All four are realtime-tunable; parameter
    // changes take effect on the next Process() call without ramp.
    constexpr uint16_t Saturation_Drive         = 19;
    constexpr uint16_t Saturation_Mix           = 20;
    constexpr uint16_t Saturation_OutputGain    = 21;
    constexpr uint16_t Saturation_Bias          = 22;
    // v0.29.0: Dattorro plate reverb additions. Decay (9) and
    // HfDamping (10) are above; these three round out the surface.
    constexpr uint16_t Reverb_PredelayMs        = 23;
    constexpr uint16_t Reverb_LfDamping         = 24;
    constexpr uint16_t Reverb_Diffusion         = 25;
    // v0.29.5: dry passthrough level. Together with WetGainDb (11) this
    // forms the standard dry/wet pair for insert use; for send/return,
    // set DryGainDb to a very negative value (e.g. -60) on the reverb
    // bus's effect so only the wet field reaches the return.
    constexpr uint16_t Reverb_DryGainDb         = 26;
    // v0.40.0: Saturation Phase 2 — mode selector. The design doc
    // (saturation_v2.md §5) claims ID 26 as the first free slot but
    // that was written before v0.29.5 took 26 for Reverb_DryGainDb;
    // ID 27 is the actual first-free at the time of v0.40.0 cut. ID
    // 28 ships as Saturation_Tone in v0.59.0 Phase 4.
    constexpr uint16_t Saturation_Mode          = 27;
    // v0.59.0: Saturation Phase 4 — tone tilt. -1..+1, smoothed
    // alongside drive/mix/bias. See saturation_v2.md §9.
    constexpr uint16_t Saturation_Tone          = 28;

    // v0.63.0: MasterControl effect (Phase 7 Master FX Lite).
    // Block reserved 30-59. See audio_engine/dsp/master_control.h
    // for stage semantics and audio_design/master_fx.md for the
    // broader design. The 6 telemetry IDs (53-58) are read-only:
    // OnParameter ignores writes to them, GetParameter returns
    // the most-recent Process() value.
    //
    // Stage enables (write 0.0 = off, !0 = on).
    constexpr uint16_t MC_GlueEnabled           = 30;
    constexpr uint16_t MC_RiderEnabled          = 31;
    constexpr uint16_t MC_LimiterEnabled        = 32;
    // Stage 1 — Glue compressor.
    constexpr uint16_t MC_GlueThresholdDb       = 33;
    constexpr uint16_t MC_GlueRatio             = 34;
    constexpr uint16_t MC_GlueAttackMs          = 35;
    constexpr uint16_t MC_GlueReleaseMs         = 36;
    constexpr uint16_t MC_GlueKneeDb            = 37;
    constexpr uint16_t MC_GlueMakeupDb          = 38;
    // Stage 3 — Gain rider (LUFS-targeted).
    constexpr uint16_t MC_RiderTargetLufs       = 39;
    constexpr uint16_t MC_RiderTimeConstMs      = 40;
    constexpr uint16_t MC_RiderMaxGainDb        = 41;
    constexpr uint16_t MC_RiderMinGainDb        = 42;
    constexpr uint16_t MC_RiderFreezeBelowLufs  = 43;
    // Stage 4 — True-peak brickwall limiter.
    constexpr uint16_t MC_LimiterCeilingDbtp    = 44;
    constexpr uint16_t MC_LimiterReleaseMs      = 45;
    constexpr uint16_t MC_LimiterLookaheadMs    = 46;
    // 47-52 reserved for future MC config (Phase B+ stages, e.g.
    // spectral suppression thresholds).
    // Telemetry (read-only). GetParameter returns current values;
    // OnParameter ignores writes.
    constexpr uint16_t MC_TelLufsShortTerm      = 53;
    constexpr uint16_t MC_TelLufsIntegrated     = 54;
    constexpr uint16_t MC_TelPeakDb             = 55;
    constexpr uint16_t MC_TelTruePeakDbtp       = 56;
    constexpr uint16_t MC_TelGainReductionDb    = 57;
    constexpr uint16_t MC_TelRiderGainDb        = 58;
    // 59 reserved.
}

// ---- Bus configuration ----------------------------------------------------

// Distance-driven gain factor applied to voices accumulating into this bus.
// Used to implement the "RemoteGunNearby" pattern from the ducking TDD: a
// silent send-only bus whose level scales with listener proximity, driving
// a sidechain compressor while contributing nothing to the audible mix.
struct ProximityCurve {
    bool  enabled        = false;
    float fullDistance   = 5.0f;    // <= this distance: full gain
    float falloffDistance = 30.0f;  // >= this distance: zero gain
    float curveExponent  = 1.5f;    // >1 = aggressive falloff near max range
};

struct BusConfig {
    BusId       id             = kInvalidBusId;
    BusId       parent         = kBusMaster;     // routes here; ignored for master
    float       outputGainDb   = 0.0f;
    bool        silent         = false;          // true: output not summed into parent (sidechain-source-only)

    // Optional name for diagnostics. Not used for routing.
    char        debugName[16]  = {0};

    ProximityCurve proximityCurve;

    // Effect chain (in order). Effects beyond effectCount are ignored.
    EffectConfig effects[kMaxEffectsPerBus];
    uint32_t     effectCount   = 0;
};

// Map from AudioCategory to a default target BusId. Used when an emitter
// doesn't override its target bus explicitly. A category mapped to
// kInvalidBusId routes that category to master.
struct CategoryBusMap {
    BusId music     = kBusMaster;
    BusId voice     = kBusMaster;
    BusId sfx       = kBusMaster;
    BusId ambience  = kBusMaster;
    BusId ui        = kBusMaster;
    BusId dialogue  = kBusMaster;
};

// Top-level bus graph configuration. Embedded in AudioConfig.
struct BusGraphConfig {
    // Buses, in any order. Master must always exist (id == kBusMaster). If
    // busCount == 0 the runtime auto-creates a single-bus graph (master only,
    // no effects) and routes every voice there.
    BusConfig buses[kMaxBuses];
    uint32_t  busCount = 0;

    CategoryBusMap categoryMap;
};

} // namespace audio

#endif // AUDIO_ENGINE_BUS_H
