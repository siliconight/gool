// audio_engine/compressor_profiles.h
//
// Curated compressor parameter bundles encoding game-audio expertise.
// Each profile is an `inline constexpr` function returning an
// `EffectConfig` populated with sensible defaults for a specific
// scenario — drum-bus punch, voice-over smoothing, music ducking
// under dialogue, etc.
//
// The single parameter every profile takes is `thresholdDb`, because
// threshold is the one value that genuinely depends on the host's
// own bus loudness target (a -10 dB threshold means very different
// things on a -6 LUFS master bus vs a -20 LUFS music sub). All other
// parameters are hard-coded per profile because they characterize the
// *intent* (punch vs glue vs duck), not the level.
//
// Hosts can override any field after the call:
//
//     auto cfg = audio::CompressorProfiles::MusicDuckUnderVoice();
//     cfg.compressorSidechainBus = kVoiceBusId;   // wire the trigger
//     cfg.compressorThresholdDb  = -25.0f;        // tune for our mix
//     bus.effects.push_back(cfg);
//
// All values are taken from standard game-audio reference settings
// (Audiokinetic Wwise default templates, FMOD Studio presets, common
// mastering-engineer rules of thumb). Where two sources disagree,
// the choice that errs on the side of "obviously musical, never
// destructive" was taken.
//
// These are starting points, not religion. Tune to your project.

#ifndef AUDIO_ENGINE_COMPRESSOR_PROFILES_H
#define AUDIO_ENGINE_COMPRESSOR_PROFILES_H

#include "audio_engine/bus.h"

namespace audio::CompressorProfiles {

// =============================================================================
// PUNCH
// =============================================================================
//
// Goal: preserve transient impact, add body / consistency. Slow-ish
// attack lets the initial peak through; mix < 1.0 layers compressed
// body under untouched dry transient ("New York" / parallel
// compression). Use these on percussive content where you want the
// attack to land hard but the sustain to feel uniform.

// Drum bus or percussion layer.
//
// Pattern: 4:1 ratio, 10 ms attack (transient passes), 80 ms release
// (body sustains), 70 % wet mix (dry transient at full level under
// the wet body), 6 dB soft knee (musical entry around threshold),
// RMS detection (averages body, ignores spike micro-detail).
// Light makeup brings the perceived loudness back up.
//
// Tune `thresholdDb` to roughly the average peak level of your drum
// bus content; -18 dB is a reasonable starting point for a bus that
// peaks around -12 dBFS.
inline constexpr EffectConfig DrumBusPunch(float thresholdDb = -18.0f) {
    EffectConfig ec{};
    ec.kind                       = EffectKind::Compressor;
    ec.compressorThresholdDb      = thresholdDb;
    ec.compressorRatio            = 4.0f;
    ec.compressorAttackMs         = 10.0f;
    ec.compressorReleaseMs        = 80.0f;
    ec.compressorMakeupDb         = 2.0f;
    ec.compressorKneeWidthDb      = 6.0f;
    ec.compressorMixRatio         = 0.7f;
    ec.compressorDetectionMode    =
        EffectConfig::CompressorDetectionMode::Rms;
    return ec;
}

// Footstep / Foley layer.
//
// Pattern: 3:1 ratio, 8 ms attack, 60 ms release, 60 % wet, 4 dB
// knee. Lighter touch than DrumBusPunch — footsteps are short,
// detail-rich, and over-compression turns them into mush.
// Detection stays on Peak because individual steps are short
// enough that RMS averaging just blurs them.
//
// Threshold default -22 dB suits a footstep bus that tends to sit
// quieter than a drum bus.
inline constexpr EffectConfig FootstepGlue(float thresholdDb = -22.0f) {
    EffectConfig ec{};
    ec.kind                       = EffectKind::Compressor;
    ec.compressorThresholdDb      = thresholdDb;
    ec.compressorRatio            = 3.0f;
    ec.compressorAttackMs         = 8.0f;
    ec.compressorReleaseMs        = 60.0f;
    ec.compressorMakeupDb         = 1.0f;
    ec.compressorKneeWidthDb      = 4.0f;
    ec.compressorMixRatio         = 0.6f;
    return ec;
}

// Single-shot weapon / transient SFX.
//
// Pattern: 4:1 ratio, 5 ms attack (catches the snap before peak),
// 100 ms release, 80 % wet, 8 dB range cap so sustained automatic
// fire can't fully duck the signal. No knee softening — gunshots
// want a firm gain envelope.
//
// Threshold -16 dB suits a weapons bus that peaks around -6 to
// -10 dBFS during fire. If your weapons run hotter, raise the
// threshold; if they run quieter, lower it.
inline constexpr EffectConfig GunshotSnap(float thresholdDb = -16.0f) {
    EffectConfig ec{};
    ec.kind                       = EffectKind::Compressor;
    ec.compressorThresholdDb      = thresholdDb;
    ec.compressorRatio            = 4.0f;
    ec.compressorAttackMs         = 5.0f;
    ec.compressorReleaseMs        = 100.0f;
    ec.compressorMakeupDb         = 0.0f;
    ec.compressorMaxReductionDb   = 8.0f;
    ec.compressorMixRatio         = 0.8f;
    return ec;
}

// =============================================================================
// IMPACT
// =============================================================================
//
// Goal: contain large dynamic events (explosions, big hits, sub-bass
// rumble) so they sit in the mix instead of crushing it. Slower
// attack-and-release shapes than punch profiles; range caps prevent
// sustained content from pinning the gain reduction; sidechain HPFs
// where appropriate so the comp doesn't trigger off its own sub.

// Explosions, large hits, mortar shells.
//
// Pattern: 5:1 ratio (firmer than punch profiles), 3 ms attack
// (fast enough to catch the leading edge), 150 ms release (longer
// recovery for the post-blast tail), 12 dB range cap (sustained
// rumble can't fully duck), 3 dB knee (firm). Peak detection — we
// want transient awareness on big hits.
//
// Threshold default -14 dB works for a sub/impact bus that peaks
// around -3 to -6 dBFS during big events.
inline constexpr EffectConfig ExplosionImpact(float thresholdDb = -14.0f) {
    EffectConfig ec{};
    ec.kind                       = EffectKind::Compressor;
    ec.compressorThresholdDb      = thresholdDb;
    ec.compressorRatio            = 5.0f;
    ec.compressorAttackMs         = 3.0f;
    ec.compressorReleaseMs        = 150.0f;
    ec.compressorMakeupDb         = 0.0f;
    ec.compressorKneeWidthDb      = 3.0f;
    ec.compressorMaxReductionDb   = 12.0f;
    return ec;
}

// Sub-bass / rumble layer.
//
// Pattern: 3:1 ratio (gentle), 15 ms attack (preserves the initial
// rumble swell), 200 ms release (long, matches sub-content
// timescales), 8 dB knee soft, sidechain HPF at 80 Hz so the
// compressor doesn't trigger off its own sub content (otherwise
// every low note triggers max compression — degenerate).
//
// Threshold -20 dB suits a sub bus that runs hot but not extreme.
inline constexpr EffectConfig BassImpact(float thresholdDb = -20.0f) {
    EffectConfig ec{};
    ec.kind                       = EffectKind::Compressor;
    ec.compressorThresholdDb      = thresholdDb;
    ec.compressorRatio            = 3.0f;
    ec.compressorAttackMs         = 15.0f;
    ec.compressorReleaseMs        = 200.0f;
    ec.compressorMakeupDb         = 0.0f;
    ec.compressorKneeWidthDb      = 8.0f;
    ec.compressorSidechainHpfHz   = 80.0f;
    return ec;
}

// =============================================================================
// DYNAMICS  /  GLUE
// =============================================================================
//
// Goal: smooth out unevenness without changing the character of
// the signal. Very low ratios; soft knees; RMS detection where the
// content rewards averaging (sustained sources like music sub-mixes,
// dialogue). These are "1-3 dB of reduction on the loudest peaks"
// profiles — set-and-forget cohesion, not corrective dynamics.

// Master bus glue — final-mix cohesion.
//
// Pattern: 1.5:1 ratio (very gentle), 30 ms attack (preserves all
// transient detail), 250 ms release (long, no pumping), 8 dB soft
// knee, RMS detection. This is the lightest touch in the library
// — meant to apply 1-2 dB of reduction on the very loudest peaks
// only. If you're seeing more than 3 dB reduction, raise the
// threshold.
//
// Default threshold -10 dB catches only the loudest peaks on a
// master bus that's been roughly leveled. Master-bus compressors
// are usually the LAST place you set threshold; do it after
// everything else is in the right place.
inline constexpr EffectConfig MasterBusGlue(float thresholdDb = -10.0f) {
    EffectConfig ec{};
    ec.kind                       = EffectKind::Compressor;
    ec.compressorThresholdDb      = thresholdDb;
    ec.compressorRatio            = 1.5f;
    ec.compressorAttackMs         = 30.0f;
    ec.compressorReleaseMs        = 250.0f;
    ec.compressorMakeupDb         = 0.5f;
    ec.compressorKneeWidthDb      = 8.0f;
    ec.compressorDetectionMode    =
        EffectConfig::CompressorDetectionMode::Rms;
    return ec;
}

// Voice-over / dialogue smoothing.
//
// Pattern: 4:1 ratio, 5 ms attack (catches sibilance), 80 ms
// release, 6 dB soft knee, 30 ms hold (no pumping between
// syllables — release won't engage until 30 ms of below-threshold
// signal, so brief inter-word silences don't trigger gain bounce),
// RMS detection (matches how perceived speech loudness is judged).
//
// Threshold -18 dB suits a dialogue bus running at typical -23 LUFS
// integrated. Adjust if your VO is hotter or quieter.
inline constexpr EffectConfig VoiceSmoothing(float thresholdDb = -18.0f) {
    EffectConfig ec{};
    ec.kind                       = EffectKind::Compressor;
    ec.compressorThresholdDb      = thresholdDb;
    ec.compressorRatio            = 4.0f;
    ec.compressorAttackMs         = 5.0f;
    ec.compressorReleaseMs        = 80.0f;
    ec.compressorMakeupDb         = 1.5f;
    ec.compressorKneeWidthDb      = 6.0f;
    ec.compressorHoldMs           = 30.0f;
    ec.compressorDetectionMode    =
        EffectConfig::CompressorDetectionMode::Rms;
    return ec;
}

// =============================================================================
// SIDECHAIN  DUCKERS
// =============================================================================
//
// Goal: lower one bus when another bus has signal — the canonical
// "music ducks under voice" pattern. These profiles configure the
// compressor for ducking but leave `compressorSidechainBus` at the
// default (kInvalidBusId, = self-sidechain). The host MUST set the
// trigger bus before installing or the comp will self-sidechain
// (which on a music bus means "music ducks itself," which is
// useless).
//
//     auto cfg = audio::CompressorProfiles::MusicDuckUnderVoice();
//     cfg.compressorSidechainBus = kVoiceBusId;     // <-- required
//     bus.effects.push_back(cfg);
//
// Sidechain HPFs are non-zero on these because the duck-trigger
// signal usually contains low-frequency content that shouldn't
// drive the compressor (breath/pop on voice; sub on SFX).

// Music ducks under voice / dialogue.
//
// Pattern: 8:1 ratio (assertive — voice should clearly win), 5 ms
// attack (voice intelligibility starts immediately), 200 ms
// release (smooth recovery between phrases), 12 dB range cap
// (deep duck without going to silence), sidechain HPF at 200 Hz
// (ignore breath, plosives, sub content in the voice signal —
// only the speech body triggers the duck), 6 dB soft knee.
//
// Threshold -22 dB on the *trigger signal* — when voice exceeds
// -22 dB on the voice bus, music starts ducking. Tune this to
// roughly match where dialogue typically sits.
inline constexpr EffectConfig MusicDuckUnderVoice(float thresholdDb = -22.0f) {
    EffectConfig ec{};
    ec.kind                       = EffectKind::Compressor;
    ec.compressorThresholdDb      = thresholdDb;
    ec.compressorRatio            = 8.0f;
    ec.compressorAttackMs         = 5.0f;
    ec.compressorReleaseMs        = 200.0f;
    ec.compressorMakeupDb         = 0.0f;
    ec.compressorKneeWidthDb      = 6.0f;
    ec.compressorMaxReductionDb   = 12.0f;
    ec.compressorSidechainHpfHz   = 200.0f;
    ec.compressorDetectionMode    =
        EffectConfig::CompressorDetectionMode::Rms;
    // compressorSidechainBus left at kInvalidBusId — host must set.
    return ec;
}

// Music ducks under SFX / weapons / explosions.
//
// Pattern: 6:1 ratio (less assertive than the voice duck — SFX
// shouldn't bury the music as deep), 8 ms attack (slightly slower
// — SFX transients are short and we don't need to clamp instantly),
// 250 ms release (long-tail recovery after sustained fire), 9 dB
// range cap (less deep than voice duck), sidechain HPF at 150 Hz
// (ignore explosion sub-bass that would trigger a constant deep
// duck), 4 dB soft knee.
//
// Threshold -18 dB on the SFX bus.
inline constexpr EffectConfig MusicDuckUnderSfx(float thresholdDb = -18.0f) {
    EffectConfig ec{};
    ec.kind                       = EffectKind::Compressor;
    ec.compressorThresholdDb      = thresholdDb;
    ec.compressorRatio            = 6.0f;
    ec.compressorAttackMs         = 8.0f;
    ec.compressorReleaseMs        = 250.0f;
    ec.compressorMakeupDb         = 0.0f;
    ec.compressorKneeWidthDb      = 4.0f;
    ec.compressorMaxReductionDb   = 9.0f;
    ec.compressorSidechainHpfHz   = 150.0f;
    ec.compressorDetectionMode    =
        EffectConfig::CompressorDetectionMode::Rms;
    // compressorSidechainBus left at kInvalidBusId — host must set.
    return ec;
}

} // namespace audio::CompressorProfiles

#endif // AUDIO_ENGINE_COMPRESSOR_PROFILES_H
