// audio_engine/saturation_profiles.h
//
// Curated saturation parameter bundles. Sibling to
// `compressor_profiles.h`. Same shape: each profile is one
// `inline constexpr` function returning a populated `EffectConfig`
// with `kind = EffectKind::Saturation` and the saturation fields
// set per the recipe.
//
// The intent is *light* enhancement — engine-side saturation is for
// glue and impact reinforcement, not the heavy distortion you'd
// shape in a DAW. Anything more aggressive, do offline and re-import.
//
// v0.40.0 update: drive is now normalized 0..1 and mapped per-mode
// internally (Tanh range 1..4, Tube/Tape 1..3, Diode 1..6). All
// profiles below pin `saturationMode = 0` (Tanh) and use the
// round-tripped normalized drive value, so their sound is bit-for-bit
// identical to v0.39.x. New profiles or per-project tuning can opt
// into the other modes (Tube for dialogue, Tape for music, Diode
// for radio/comms) — see saturation_v2.md §6.1.
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
// Pattern: norm drive 0.167 (Tanh maps to scale 1.5 — gentle harmonic
// generation), mix 15 % wet (audible cohesion, no character change),
// output gain 0.85 (compensates for the small loudness boost drive
// introduces), no bias (symmetric — odd harmonics only, transparent).
//
// Use on a master or a music sub-bus. If you can hear it as a
// separate effect, dial mix down.
inline constexpr EffectConfig BusGlue() {
    EffectConfig ec{};
    ec.kind                  = EffectKind::Saturation;
    ec.saturationDrive       = 0.1667f;   // Tanh scale 1.5 (= 1 + 3·0.1667)
    ec.saturationMix         = 0.15f;
    ec.saturationOutputGain  = 0.85f;
    ec.saturationBias        = 0.0f;
    ec.saturationMode        = 0;         // Tanh — v0.40.0 default
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
// Pattern: norm drive 0.10 (Tanh scale 1.3 — very light), mix 10 %
// wet (subtle), bias 0.05 (small DC offset → asymmetric → even
// harmonics → "tube"-like warmth), output gain 0.9.
//
// v0.40.0 tuning note: SaturationMode::Tube would arguably suit
// dialogue better (the asinh shoulder is gentler than tanh's), but
// switching modes changes the character; this profile keeps Tanh
// for round-trip compat. Try Tube if your dialogue chain wants a
// hair more openness.
//
// If the source already has DAW-side analog modeling, set mix to
// zero and skip this effect — stacking warmth processors gets muddy.
inline constexpr EffectConfig DialogueWarmth() {
    EffectConfig ec{};
    ec.kind                  = EffectKind::Saturation;
    ec.saturationDrive       = 0.10f;     // Tanh scale 1.3 (= 1 + 3·0.10)
    ec.saturationMix         = 0.10f;
    ec.saturationOutputGain  = 0.9f;
    ec.saturationBias        = 0.05f;
    ec.saturationMode        = 0;         // Tanh — v0.40.0 default
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
// Pattern: norm drive 0.5 (Tanh scale 2.5 — assertive harmonic
// generation), mix 30 % wet (clearly audible character), output
// gain 0.7 (significant trim — scale 2.5 boosts perceived loudness
// 3–4 dB), symmetric.
//
// v0.40.0 tuning note: SaturationMode::Diode is purpose-built for
// "gunshot bite" character (sharp cubic shoulder). Try it for
// dryer, more aggressive impacts; Tanh stays the safe round-trip
// default. Best paired with `CompressorProfiles::GunshotSnap`
// after the saturator: saturate first to add body, compress to
// control the boosted peaks.
inline constexpr EffectConfig WeaponBody() {
    EffectConfig ec{};
    ec.kind                  = EffectKind::Saturation;
    ec.saturationDrive       = 0.5f;      // Tanh scale 2.5 (= 1 + 3·0.5)
    ec.saturationMix         = 0.30f;
    ec.saturationOutputGain  = 0.7f;
    ec.saturationBias        = 0.0f;
    ec.saturationMode        = 0;         // Tanh — v0.40.0 default
    return ec;
}

// Explosion / impact character — large hits with grit.
//
// Pattern: norm drive 1.0 (Tanh scale 4.0 — max Tanh range,
// pushing the shape into clear nonlinearity), mix 45 % wet, bias
// 0.10 (asymmetry → even harmonics → "broken speaker" feel that
// movie hits rely on), output gain 0.55 (heavy trim).
//
// Pair with `CompressorProfiles::ExplosionImpact` for the full
// "movie hit" chain.
inline constexpr EffectConfig ImpactCharacter() {
    EffectConfig ec{};
    ec.kind                  = EffectKind::Saturation;
    ec.saturationDrive       = 1.0f;      // Tanh scale 4.0 (= 1 + 3·1.0)
    ec.saturationMix         = 0.45f;
    ec.saturationOutputGain  = 0.55f;
    ec.saturationBias        = 0.10f;
    ec.saturationMode        = 0;         // Tanh — v0.40.0 default
    return ec;
}

// =============================================================================
// TAPE  COLOR
// =============================================================================

// Tape-style color for music / ambience.
//
// Pattern: norm drive 0.333 (Tanh scale 2.0 — medium), mix 25 % wet
// (audible character — the "not-digital" feel that listeners
// associate with analog recording), output gain 0.75, symmetric.
//
// v0.40.0 tuning note: this profile pre-dates the actual Tape mode.
// SaturationMode::Tape is the literal Zölzer soft-quadratic that
// approximates magnetic-tape behavior; switching this profile to
// mode 2 would arguably be a better match for the name. Kept on
// Tanh for round-trip compat. Pair with `MasterBusGlue`
// compressor downstream so the harmonics get gently leveled by
// RMS averaging — which is how analog tape and tape compressors
// actually interacted.
inline constexpr EffectConfig TapeColor() {
    EffectConfig ec{};
    ec.kind                  = EffectKind::Saturation;
    ec.saturationDrive       = 0.3333f;   // Tanh scale 2.0 (= 1 + 3·0.3333)
    ec.saturationMix         = 0.25f;
    ec.saturationOutputGain  = 0.75f;
    ec.saturationBias        = 0.0f;
    ec.saturationMode        = 0;         // Tanh — v0.40.0 default
    return ec;
}

} // namespace audio::SaturationProfiles

#endif // AUDIO_ENGINE_SATURATION_PROFILES_H
