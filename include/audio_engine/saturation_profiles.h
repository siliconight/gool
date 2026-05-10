// audio_engine/saturation_profiles.h
//
// Curated saturation parameter bundles. Sibling to
// `compressor_profiles.h`. Same shape: each profile is one
// `inline constexpr` function returning a populated `EffectConfig`
// with `kind = EffectKind::Saturation` and the four saturation
// fields set per the recipe.
//
// The intent is *light* enhancement — engine-side saturation is for
// glue and impact reinforcement, not the heavy distortion you'd
// shape in a DAW. Drive values stay in 1.3–4.0; mixes in 0.10–0.45.
// Anything more aggressive, do offline and re-import.
//
//     #include "audio_engine/saturation_profiles.h"
//
//     // Light bus glue on the master:
//     bus.effects.push_back(audio::SaturationProfiles::BusGlue());
//
//     // Override anything after the call:
//     auto cfg = audio::SaturationProfiles::ImpactCharacter();
//     cfg.saturationMix = 0.30f;   // dial back from preset 0.45
//     bus.effects.push_back(cfg);
//
// Numbers are starting points taken from common
// production-engineering rules of thumb. Tune to your project.

#ifndef AUDIO_ENGINE_SATURATION_PROFILES_H
#define AUDIO_ENGINE_SATURATION_PROFILES_H

#include "audio_engine/bus.h"

namespace audio::SaturationProfiles {

// =============================================================================
// BUS  GLUE
// =============================================================================
//
// The everyday "make things sit together" tool. Subtle harmonic
// fattening that's audible on the full mix but invisible on any
// individual element. Drop on a master bus or sub-bus and forget.

// Light master-bus glue.
//
// Pattern: drive 1.5 (gentle harmonic generation), mix 15 % wet
// (audible cohesion, no character change), output gain 0.85
// (compensates for the small loudness boost drive introduces),
// no bias (symmetric — odd harmonics only, transparent).
//
// Use on a master or a music sub-bus. If you can hear it as a
// separate effect, dial mix down.
inline constexpr EffectConfig BusGlue() {
    EffectConfig ec{};
    ec.kind                  = EffectKind::Saturation;
    ec.saturationDrive       = 1.5f;
    ec.saturationMix         = 0.15f;
    ec.saturationOutputGain  = 0.85f;
    ec.saturationBias        = 0.0f;
    return ec;
}

// =============================================================================
// DIALOGUE  WARMTH
// =============================================================================
//
// Light asymmetric saturation tuned for voice — adds the kind of
// even-harmonic warmth a real-world signal chain would impart on a
// preamp. Designed to sit on a dialogue bus after the
// `VoiceSmoothing` compressor, before any reverb send.

// Voice / dialogue warmth.
//
// Pattern: drive 1.3 (very light), mix 10 % wet (subtle), bias 0.05
// (small DC offset → asymmetric → even harmonics → "tube"-like
// warmth), output gain 0.9 (compensates).
//
// If the source already has DAW-side analog modeling, set mix to
// zero and skip this effect — stacking warmth processors gets muddy.
inline constexpr EffectConfig DialogueWarmth() {
    EffectConfig ec{};
    ec.kind                  = EffectKind::Saturation;
    ec.saturationDrive       = 1.3f;
    ec.saturationMix         = 0.10f;
    ec.saturationOutputGain  = 0.9f;
    ec.saturationBias        = 0.05f;
    return ec;
}

// =============================================================================
// HIT  CHARACTER
// =============================================================================
//
// Stronger saturation profiles for percussive content. These don't
// belong on a sub-bus that mixes many sources — install on a
// dedicated weapon / impact bus and dial mix to taste.

// Weapon body — gunshots, melee impacts, single-shot SFX.
//
// Pattern: drive 2.5 (assertive harmonic generation — adds body
// where the dry sample lacks bottom), mix 30 % wet (clearly
// audible character), output gain 0.7 (significant trim — drive
// at 2.5 boosts perceived loudness 3–4 dB), symmetric.
//
// Best paired with `CompressorProfiles::GunshotSnap` *after* the
// saturator: saturate first to add body, compress to control the
// boosted peaks.
inline constexpr EffectConfig WeaponBody() {
    EffectConfig ec{};
    ec.kind                  = EffectKind::Saturation;
    ec.saturationDrive       = 2.5f;
    ec.saturationMix         = 0.30f;
    ec.saturationOutputGain  = 0.7f;
    ec.saturationBias        = 0.0f;
    return ec;
}

// Explosion / impact character — large hits with grit.
//
// Pattern: drive 4.0 (heavy harmonic generation — pushing tanh
// into clear nonlinearity), mix 45 % wet (close to fully
// saturated — the dry stays just present enough to keep the
// transient pop), bias 0.10 (asymmetry — even harmonics give a
// "broken speaker" / "blown subwoofer" feel that big movie hits
// rely on), output gain 0.55 (heavy trim — drive at 4.0 boosts
// peak loudness substantially and we want to fit back into the
// bus's headroom).
//
// Pair with `CompressorProfiles::ExplosionImpact` for the full
// "movie hit" chain.
inline constexpr EffectConfig ImpactCharacter() {
    EffectConfig ec{};
    ec.kind                  = EffectKind::Saturation;
    ec.saturationDrive       = 4.0f;
    ec.saturationMix         = 0.45f;
    ec.saturationOutputGain  = 0.55f;
    ec.saturationBias        = 0.10f;
    return ec;
}

// =============================================================================
// TAPE  COLOR
// =============================================================================

// Tape-style color for music / ambience.
//
// Pattern: drive 2.0 (medium), mix 25 % wet (audible character —
// the "not-digital" feel that listeners associate with analog
// recording), output gain 0.75, symmetric (odd-harmonic only —
// real magnetic tape's saturation curve is close to symmetric
// soft clipping).
//
// Use on a music sub-bus or ambience layer. Strongly recommended
// to pair with a soft `MasterBusGlue` compressor downstream so
// the harmonics get gently leveled by RMS averaging — which is
// how analog tape and tape compressors actually interacted.
inline constexpr EffectConfig TapeColor() {
    EffectConfig ec{};
    ec.kind                  = EffectKind::Saturation;
    ec.saturationDrive       = 2.0f;
    ec.saturationMix         = 0.25f;
    ec.saturationOutputGain  = 0.75f;
    ec.saturationBias        = 0.0f;
    return ec;
}

} // namespace audio::SaturationProfiles

#endif // AUDIO_ENGINE_SATURATION_PROFILES_H
