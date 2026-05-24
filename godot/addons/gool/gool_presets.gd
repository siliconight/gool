# addons/gool/gool_presets.gd
#
# Named effect-parameter presets that ship with gool, designed so a
# Godot dev can pick a space by name ("Cathedral") instead of dialing
# in six numbers. Each constant is a Dictionary of parameter values
# you pass to a gool apply_*_preset() helper.
#
# Example:
#   # In your scene's _ready(), after Gool is initialized:
#   Gool.apply_reverb_preset("Sfx", 0, GoolPresets.REVERB_CATHEDRAL)
#
# The dictionary keys match the JSON keys you'd write by hand in
# gool/config.json — so what you see in autocomplete is what you'd
# see in the config file. Same vocabulary, different surface.
#
# Two axes of reverb selection in gool, not to be confused:
#
#   - SPACE presets (this file): "the scene is set in a cathedral",
#     authored by you in your scene script or wired through a
#     ReverbZone. That's what these REVERB_* constants are.
#
#   - MATERIAL presets (already built in): "the bullet hit concrete",
#     selected automatically by the AudioMaterial of the collider.
#     Surfaced via Gool.get_reverb_preset_for_material(material).
#
# Both write to the same six reverb parameters. They don't conflict
# because you typically apply one or the other for a given bus —
# space presets sit on a fixed listener-reverb bus, material presets
# drive per-impact reverb sends.
#
# Tweaking and customizing:
#   You can override individual parameters after applying a preset —
#   for example, apply LARGE_HALL then call set_effect_parameter to
#   shorten the predelay. Or pass your own Dictionary in the same
#   shape; nothing requires the constants in this file.
#
# Adding your own presets in your project:
#   Just declare your own Dictionary const with the same shape:
#     const MY_HAUNTED_LIBRARY: Dictionary = {
#         "predelay_ms": 40.0, "decay": 0.78, ...
#     }
#   then pass it to Gool.apply_reverb_preset(). No gool change
#   required — the keys are checked at apply time, not bake time.

@tool
class_name GoolPresets
extends RefCounted

# ─── Reverb space presets ─────────────────────────────────────
#
# Eight parameters in every reverb dictionary. The ranges match what
# the C++ reverb effect accepts; values outside the range get
# clamped at apply time. Quick guide to what each number does to
# what you hear:
#
#   predelay_ms (0–200): how long after the dry sound before the
#     wet tail starts. Bigger numbers = the room sounds bigger.
#   decay (0–1): how long the wet tail lasts. 0 = no tail; 1 ≈ a
#     stone cathedral.
#   lf_damping (0–1): how fast low frequencies die in the tail.
#     Higher = the tail loses bass over time (deep room / cave).
#   hf_damping (0–1): how fast high frequencies die in the tail.
#     Higher = the tail loses sparkle (carpets, fabric, foliage).
#   diffusion (0–1): how blurred / scattered the tail is. Low =
#     discrete echoey slaps; high = smooth wash.
#   wet_gain_db (-24 to +12): overall loudness of the reverb. 0 dB
#     is "as authored"; negative dips the tail under the dry signal.
#
# v0.47.0 added two static-EQ-shape parameters that work together
# with damping (which is time-varying) to set the reverb's place in
# the mix. These are recipes from Sound on Sound's classic article
# on reverb realism and MixingLessons' Abbey Road write-up — both
# converge on the same principle: a real space's reflected field
# has less HF content than the dry source, and clearing LF from
# the send before reverb keeps the tail out of the bass mud.
#
#   send_hpf_hz (0–2000): HPF cutoff applied to the bus signal
#     BEFORE the Reverb effect. 0 = bypass. Typical values:
#     200 (warmth), 400 (clarity), 600 (Abbey Road).
#   return_lpf_hz (2000–22000): LPF cutoff applied AFTER the Reverb
#     effect. 22000 = bypass. The single biggest realism knob per
#     SoS — real concert halls have ~nothing above 5kHz. Typical:
#     3000 (warm/intimate), 5000–6000 (natural), 7000–10000 (bright).
#
# Note: ReverbZone auto-discovers adjacent Biquad slots in the bus
# chain at scene start. If your bus doesn't have Biquads before
# and after the Reverb effect (gool's DEFAULT_CONFIG ships them as
# of v0.47.0), these two values are silently ignored. Add the
# slots to your config.json — see docs/audio_design/reverb_eq.md.

## Tiny indoor space — bedroom, small office, car interior.
## Short predelay, quick decay, mid HF rolloff. Subtle; sets place
## without coloring the dry sound much.
const REVERB_SMALL_ROOM: Dictionary = {
	"predelay_ms":   8.0,
	"decay":         0.35,
	"lf_damping":    0.10,
	"hf_damping":    0.40,
	"diffusion":     0.70,
	# v0.69.2 dial-down: -3.0 → -5.0. Even small rooms shouldn't
	# read as "featured" reverb in a typical game mix — the level
	# cue should sit underneath the dry signal, not next to it.
	"wet_gain_db":  -5.0,
	# v0.47.0 EQ shaping — mapped from Sound on Sound and
	# MixingLessons article recipes. Game-audio context tweaked.
	"send_hpf_hz":   150.0,
	"return_lpf_hz": 10000.0,
}

## Living-room-sized interior — typical home or classroom.
## A bit more presence than SMALL_ROOM, still firmly "indoors."
const REVERB_MEDIUM_ROOM: Dictionary = {
	"predelay_ms":  20.0,
	# v0.69.2 dial-down: 0.55 → 0.48. A typical living room has a
	# ~0.4-0.6s RT60; 0.55 normalized was overshooting toward
	# "warehouse-but-furnished" character. 0.48 reads more like
	# "interior with soft furnishings."
	"decay":         0.48,
	"lf_damping":    0.15,
	"hf_damping":    0.35,
	"diffusion":     0.70,
	# v0.69.2 dial-down: -2.0 → -5.0. Same reasoning as SMALL_ROOM.
	"wet_gain_db":  -5.0,
	# v0.47.0 EQ shaping — mapped from Sound on Sound and
	# MixingLessons article recipes. Game-audio context tweaked.
	"send_hpf_hz":   200.0,
	"return_lpf_hz": 7000.0,
}

## Big enclosed space — concert hall, gymnasium, warehouse.
## Clear tail with some sparkle preserved.
const REVERB_LARGE_HALL: Dictionary = {
	"predelay_ms":  50.0,
	# v0.69.2 dial-down: 0.80 → 0.70. Concert halls do have long
	# tails (~1.8-2.5s RT60), but on a normalized scale 0.80 was
	# pushing into "Royal Albert Hall during a long-decay piece"
	# territory. 0.70 sits in "medium-large performance space"
	# which is more useful as a general-purpose hall preset.
	"decay":         0.70,
	"lf_damping":    0.20,
	"hf_damping":    0.25,
	# v0.46.1 retune: 0.65 → 0.78. Concert/event halls have soft
	# furnishings (seats, drapes, audience absorption) scattering
	# reflections, which perceptually maps to higher diffusion than
	# the previous 0.65. 0.78 sits between bathroom's flutter
	# character (0.45) and cathedral's full wash (0.85) — gives the
	# preset a distinct "spacious but defined" tail.
	"diffusion":     0.78,
	# v0.69.2 dial-down: -1.0 → -5.0. Halls in a mix usually sit
	# 4-6 dB below dry to preserve dialogue/source clarity.
	"wet_gain_db":  -5.0,
	# v0.47.0 EQ shaping — mapped from Sound on Sound and
	# MixingLessons article recipes. Game-audio context tweaked.
	"send_hpf_hz":   250.0,
	"return_lpf_hz": 6000.0,
}

## Massive stone interior — cathedral, mausoleum, abandoned
## subway station. Long predelay + long decay; tail dwarfs the
## dry sound. Use sparingly; on dialogue it'll smear consonants.
const REVERB_CATHEDRAL: Dictionary = {
	"predelay_ms": 100.0,
	# v0.69.2 dial-down: 0.92 → 0.85. Still by far the longest tail
	# in the set — Cathedral is supposed to be extreme. 0.92 was
	# saturating into "audibly-infinite" though; 0.85 keeps the
	# multi-second tail but lets it resolve enough that subsequent
	# sounds don't pile up indefinitely.
	"decay":         0.85,
	"lf_damping":    0.30,
	# v0.46.1: bumped 0.20 → 0.18 to keep just a hint more brightness
	# in the tail (cathedrals are stone — they don't absorb HF as
	# aggressively as upholstered halls).
	"hf_damping":    0.18,
	# v0.46.1 retune: 0.55 → 0.85. Diffusion 0.55 produces audibly
	# discrete reflections — corridor or stairwell character, not a
	# cathedral's wash. Real cathedrals have irregular vaults +
	# columns + pews scattering reflections at every density, which
	# perceptually maps to high diffusion (smooth tail). Bringing
	# this up is the biggest single perceptual win on the preset set.
	"diffusion":     0.85,
	# v0.69.2 dial-down: 0.0 → -4.0. Cathedral at unity wet against
	# dry was the most extreme cell in the set — the reverb was as
	# loud as the source. -4 dB still makes the cathedral feel huge
	# without the tail competing with the dry signal for foreground.
	"wet_gain_db":  -4.0,
	# v0.47.0 EQ shaping — mapped from Sound on Sound and
	# MixingLessons article recipes. Game-audio context tweaked.
	"send_hpf_hz":   300.0,
	"return_lpf_hz": 6000.0,
}

## Cavern / mineshaft / damp tunnel. Heavy LF + HF damping makes
## the tail "dark" and scattered; high diffusion smooths it. Reads
## as natural-stone enclosure rather than built architecture.
const REVERB_CAVE: Dictionary = {
	"predelay_ms":  30.0,
	# v0.69.2 dial-down: 0.88 → 0.75. Real caves have long tails
	# but the irregular geometry + heavy LF/HF damping should make
	# them sound enclosed rather than endless. 0.88 was reading
	# closer to "abandoned subway tunnel that goes for kilometers";
	# 0.75 with the existing damping reads as a contained cavern.
	"decay":         0.75,
	"lf_damping":    0.40,
	"hf_damping":    0.55,
	"diffusion":     0.80,
	# v0.69.2 dial-down: -2.0 → -5.0. Same as the other long-tail
	# presets — recede the tail under the dry signal.
	"wet_gain_db":  -5.0,
	# v0.47.0 EQ shaping — mapped from Sound on Sound and
	# MixingLessons article recipes. Game-audio context tweaked.
	"send_hpf_hz":   200.0,
	"return_lpf_hz": 4500.0,
}

## Hard, parallel-walled small room with reflective surfaces —
## bathroom, swimming-pool changing room, tiled corridor. Very
## little damping; low diffusion makes the early reflections
## audible as slapback.
const REVERB_BATHROOM_TILE: Dictionary = {
	# v0.46.1: kept predelay short (5ms) — close walls = early
	# reflection arrives ~immediately. Matches the perceptual cue
	# for "small tight space."
	"predelay_ms":   5.0,
	# v0.46.1 retune: 0.65 → 0.50. Real bathrooms are 3-4m³ — the
	# tail length is ~0.6-1.0s, which on gool's normalized [0..1]
	# decay maps to ~0.45-0.55. 0.65 was reading high-school-locker-
	# room. The flutter-echo character (low diffusion, low damping)
	# was correct; just the duration was overshooting.
	# v0.69.2 dial-down: 0.50 → 0.45. Snapping the tail a touch
	# shorter so the flutter character is heard but doesn't ring on.
	"decay":         0.45,
	"lf_damping":    0.05,
	"hf_damping":    0.10,
	"diffusion":     0.45,
	# v0.69.2 dial-down: 0.0 → -3.0. Tiled surfaces ARE reflective
	# but at unity wet the preset was a one-note effect; dropping
	# 3dB lets the flutter sit underneath the dry signal.
	"wet_gain_db":  -3.0,
	# v0.47.0 EQ shaping — mapped from Sound on Sound and
	# MixingLessons article recipes. Game-audio context tweaked.
	"send_hpf_hz":   200.0,
	"return_lpf_hz": 5000.0,
}

## Open exterior — field, meadow, rooftop. Almost no enclosure, so
## the "reverb" is mostly an absorption tail rather than reflection.
## Very low decay, high diffusion, strong HF damping (air).
## Wet level dropped well below dry — outdoors should never feel
## reverberant, just "not anechoic".
const REVERB_OUTDOOR_OPEN: Dictionary = {
	"predelay_ms":   0.0,
	"decay":         0.20,
	"lf_damping":    0.50,
	"hf_damping":    0.70,
	"diffusion":     0.95,
	# v0.69.2 dial-down: -8.0 → -12.0. Outdoor should feel "not
	# anechoic" — a thin sense of air absorption and ground-bounce
	# scattering — but reverb at -8 dB was reading as a perceptible
	# tail, which doesn't match real outdoor acoustics. -12 dB is
	# the "barely there" floor that still anchors the spatial cue.
	"wet_gain_db": -12.0,
	# v0.47.0 EQ shaping — mapped from Sound on Sound and
	# MixingLessons article recipes. Game-audio context tweaked.
	"send_hpf_hz":   100.0,
	"return_lpf_hz": 4000.0,
}

## Submerged / stylistic underwater. Extreme LF and HF damping
## leave the tail squeezed into a narrow mid band, which the ear
## reads as muffled-by-water. Combine with a lowpass on the dry
## path (e.g. via the ListenerEq bus) for full effect.
const REVERB_UNDERWATER: Dictionary = {
	"predelay_ms":  60.0,
	# v0.69.2 dial-down: 0.75 → 0.65. Underwater muffling is mostly
	# carried by the HF damping (0.95) and the post-LPF (2500 Hz);
	# the long tail at 0.75 was adding "deep cavern under the
	# ocean" character. 0.65 keeps the stylistic feel without
	# the cavern-tail.
	"decay":         0.65,
	"lf_damping":    0.80,
	"hf_damping":    0.95,
	"diffusion":     0.90,
	# v0.69.2 dial-down: -3.0 → -6.0. Same as the long-tail set.
	"wet_gain_db":  -6.0,
	# v0.47.0 EQ shaping — mapped from Sound on Sound and
	# MixingLessons article recipes. Game-audio context tweaked.
	"send_hpf_hz":   100.0,
	"return_lpf_hz": 2500.0,
}

# ─── Compressor presets ────────────────────────────────────────
#
# Sidechain compression for ducking — typically "Music ducks under
# Sfx so weapon shots feel punchy without burying the music." Values
# ported from docs/audio_design/sidechain_tuning.md where each preset
# has its own behavior writeup (when to use, what tradeoffs).
#
# These presets DO NOT set sidechain_bus — that's structural (wired
# in the bus graph itself, in gool/config.json) and the preset only
# tunes how aggressively the compressor responds once the sidechain
# is connected.
#
# Six parameters in every compressor dictionary:
#   threshold_db:    dB level at which compression starts. Lower =
#     more aggressive (catches more transients).
#   ratio:           compression ratio above threshold. 4 = "for every
#     4 dB over, 1 dB comes out"; 8 = harder squash.
#   attack_ms:       how fast the compressor reaches full reduction
#     once the threshold is crossed. Shorter = catches transients;
#     longer = lets them through.
#   release_ms:      how fast it recovers once the sidechain drops
#     below threshold. Shorter = punchy; longer = sustained duck.
#   knee_width_db:   soft-knee region around threshold for natural
#     transition. Bigger = more gradual entry into compression.
#   max_reduction_db: ceiling on how much the compressor will pull
#     down. Stops the duck from becoming silence in heavy fire.

## Default for cinematic PvE / co-op shooter — Helldivers 2 / DRG /
## L4D2 territory. Single gunshot ducks music ~6-8 dB, releases over
## 200 ms; rapid fire settles into ~6 dB continuous duck.
const COMPRESSOR_ACTION_SHOOTER: Dictionary = {
	"threshold_db":      -20.0,
	"ratio":               6.0,
	"attack_ms":           5.0,
	"release_ms":        200.0,
	"knee_width_db":       4.0,
	"max_reduction_db":   10.0,
}

## Games with sustained explosion bodies (1-3 sec decay), grenade
## launchers, missile impacts. Longer release keeps music ducked
## through the explosion's body. Tradeoff: quieter SFX (footsteps,
## pings) won't compete cleanly during recovery — best when
## explosions are sparse.
const COMPRESSOR_CINEMATIC_EXPLOSION: Dictionary = {
	"threshold_db":      -22.0,
	"ratio":               8.0,
	"attack_ms":           8.0,
	"release_ms":        600.0,
	"knee_width_db":       6.0,
	"max_reduction_db":   12.0,
}

## Stealth or single-shot dramatic — sniper rifles, narrative beats.
## Lower threshold + fast release so a single transient ducks
## dramatically then recovers quickly. Tradeoff: more pumping-prone
## if continuous sounds (footsteps, ambient noise) trip the threshold;
## best when the Sfx bus is reserved for discrete events.
const COMPRESSOR_STEALTH_SINGLE_SHOT: Dictionary = {
	"threshold_db":      -25.0,
	"ratio":               8.0,
	"attack_ms":           3.0,
	"release_ms":        120.0,
	"knee_width_db":       2.0,
	"max_reduction_db":   12.0,
}

## Constant-action / horde survival — continuous gunfire, top-down
## shooters. Higher threshold so only the loudest impacts duck,
## preventing music from being stuck at -10 dB the whole mission.
## Background fire passes through; rockets and bosses still get
## reaction.
const COMPRESSOR_HORDE_MODE: Dictionary = {
	"threshold_db":      -12.0,
	"ratio":               4.0,
	"attack_ms":          10.0,
	"release_ms":        150.0,
	"knee_width_db":       6.0,
	"max_reduction_db":    6.0,
}

## Near-imperceptible ducking — when the sound designer's mix is
## already balanced and you want compression to be "felt not heard."
## Music only dips 2-4 dB on the loudest impacts.
const COMPRESSOR_TRANSPARENT: Dictionary = {
	"threshold_db":      -10.0,
	"ratio":               3.0,
	"attack_ms":          15.0,
	"release_ms":        250.0,
	"knee_width_db":       8.0,
	"max_reduction_db":    4.0,
}

# ─── EQ presets ────────────────────────────────────────────────
#
# Tonal coloration. Each preset targets the standard 3-biquad
# LowShelf / Peak / HighShelf chain that gool's built-in EQ buses
# (ImpactEq, ListenerEq) use. The keys are the logical "what kind
# of shape" parameters; Gool.apply_eq_preset maps them to the three
# biquads' cutoff_hz / q / biquad_gain_db.
#
# If your project has a custom EQ bus with a different biquad count
# or order, pass override indices to apply_eq_preset (see its
# docstring for the signature).
#
# Seven parameters in every EQ dictionary:
#   low_gain_db / low_freq_hz   — LowShelf band (bass shelf below knee)
#   mid_gain_db / mid_freq_hz / mid_q — Peak band (focused boost or cut)
#   high_gain_db / high_freq_hz — HighShelf band (treble shelf above knee)
#
# Same layout as the internal `MaterialEqCurve` struct, by design —
# if you want to author your own preset using a material's curve as
## a starting point, the field names line up.

## Warm character — gentle low boost, slight upper-mid presence,
## softer top. Good for music buses, dialogue with a "honey" voice,
## or content that's been recorded a bit thin and needs body.
const EQ_WARM: Dictionary = {
	"low_gain_db":   3.0,
	"low_freq_hz":   200.0,
	"mid_gain_db":   1.0,
	"mid_freq_hz":   600.0,
	"mid_q":         0.7,
	"high_gain_db": -2.0,
	"high_freq_hz":  6000.0,
}

## Bright character — slight bass thin, presence in upper-mids, lift
## on top. Good for content that needs to "cut through" the mix
## (gunshots, alerts, lead instruments). The opposite shape from
## EQ_WARM.
const EQ_BRIGHT: Dictionary = {
	"low_gain_db":  -1.0,
	"low_freq_hz":   200.0,
	"mid_gain_db":   1.0,
	"mid_freq_hz":   3000.0,
	"mid_q":         0.7,
	"high_gain_db":  3.0,
	"high_freq_hz":  8000.0,
}

## Distant / far-away — everything pulled down, slight mid scoop.
## Reads as "this sound is happening across the room" without
## actually changing the gain. Useful for off-screen ambient cues,
## NPCs talking in another room, or layering a "distant" version of
## the same source behind the close one.
const EQ_DISTANT: Dictionary = {
	"low_gain_db":  -3.0,
	"low_freq_hz":   200.0,
	"mid_gain_db":  -2.0,
	"mid_freq_hz":   1500.0,
	"mid_q":         0.7,
	"high_gain_db": -4.0,
	"high_freq_hz":  6000.0,
}

## Telephone / radio / intercom — drastic low+high cuts with a
## focused mid boost, band-limiting the signal to ~300 Hz - 4 kHz.
## The classic "over the phone" effect; also works for in-helmet
## comms, walkie-talkie, AM radio. Combine with a SATURATION_RADIO_CRUSH
## on the same bus for the full broken-comm effect.
const EQ_TELEPHONE: Dictionary = {
	"low_gain_db":  -12.0,
	"low_freq_hz":   300.0,
	"mid_gain_db":    4.0,
	"mid_freq_hz":   1500.0,
	"mid_q":          2.0,
	"high_gain_db": -12.0,
	"high_freq_hz":  4000.0,
}

# ─── Saturation presets ────────────────────────────────────────
#
# Subtle harmonic coloration to full-on distortion. Each preset
# picks a `mode` (the shape function) and a drive level, plus mix
# and bias for character. The mode integer matches the C++ side:
#
#   0 = Tanh   — symmetric soft, console / music bus glue
#   1 = Tube   — gentle warmth, asymmetric via bias
#   2 = Tape   — bounded, compression-with-distortion character
#   3 = Diode  — sharp, broken-speaker / gunshot bite
#
# Five parameters in every saturation dictionary:
#   drive        — saturation amount, 0..1 (each mode has its own
#     internal range; 0.5 means roughly the same intensity across
#     modes)
#   mix          — dry/wet, 0..1 (1.0 = fully processed, 0.0 = bypass)
#   output_gain  — post-shaper level compensation, typical 0.7..1.0
#   bias         — DC offset for asymmetric character, typical 0.0..0.3
#   mode         — int 0..3, see table above

## Subtle warmth and gentle compression — tape-machine character on
## a music bus, drum bus, or full mix. Bounded shape means even at
## high drive you get glue rather than aggressive distortion.
const SATURATION_TAPE_WARMTH: Dictionary = {
	"drive":        0.40,
	"mix":          0.70,
	"output_gain":  0.85,
	"bias":         0.15,
	"mode":         2,    # Tape
}

## Broken-speaker / lo-fi radio — heavy drive with sharp shoulder
## from the diode shape. Pairs with EQ_TELEPHONE for the full
## comms-radio effect; on its own gives weapons a "tearing" edge
## or makes ambient industrial noise feel damaged.
const SATURATION_RADIO_CRUSH: Dictionary = {
	"drive":        0.85,
	"mix":          0.85,
	"output_gain":  0.70,
	"bias":         0.0,
	"mode":         3,    # Diode
}

## Console / guitar-amp drive — strong symmetric saturation on a
## tanh shape, fully wet. Adds harmonic richness to anything that
## needs to feel pushed. Good on lead instruments, weapon layers,
## or as a final touch on the master bus for a "produced" feel.
const SATURATION_AMP_DRIVE: Dictionary = {
	"drive":        0.65,
	"mix":          1.0,
	"output_gain":  0.80,
	"bias":         0.0,
	"mode":         0,    # Tanh
}

# ─── Internal mapping ─────────────────────────────────────────
#
# JSON parameter name → C++ EffectParameter integer ID. Used by
# Gool.apply_reverb_preset to translate human-readable keys into
# the IDs set_effect_parameter expects.
#
# IDs come from include/audio_engine/bus.h (EffectParameter
# namespace). If the C++ enum changes, this table updates.
#
# Marked with a leading underscore by GDScript convention — it's
# public (GDScript has no private modifier), but the underscore
# signals "implementation detail, don't depend on this."
const _REVERB_PARAM_ID: Dictionary = {
	"predelay_ms":  23,   # EffectParameter::Reverb_PredelayMs
	"decay":         9,   # EffectParameter::Reverb_Decay
	"lf_damping":   24,   # EffectParameter::Reverb_LfDamping
	"hf_damping":   10,   # EffectParameter::Reverb_HfDamping
	"diffusion":    25,   # EffectParameter::Reverb_Diffusion
	"wet_gain_db":  11,   # EffectParameter::Reverb_WetGainDb
	"dry_gain_db":  26,   # EffectParameter::Reverb_DryGainDb
}

# JSON parameter name → EffectParameter ID for compressors.
# Mirrors include/audio_engine/bus.h Compressor_* IDs.
const _COMPRESSOR_PARAM_ID: Dictionary = {
	"threshold_db":      4,    # Compressor_ThresholdDb
	"ratio":             5,    # Compressor_Ratio
	"attack_ms":         6,    # Compressor_AttackMs
	"release_ms":        7,    # Compressor_ReleaseMs
	"makeup_db":         8,    # Compressor_MakeupDb
	"knee_width_db":    13,    # Compressor_KneeWidthDb
	"mix_ratio":        14,    # Compressor_MixRatio
	"max_reduction_db": 15,    # Compressor_MaxReductionDb
	"sidechain_hpf_hz":16,    # Compressor_SidechainHpfHz
	"hold_ms":         17,    # Compressor_HoldMs
	"detection_mode":  18,    # Compressor_DetectionMode (int)
}

# JSON parameter name → EffectParameter ID for biquads. Each EQ
# band is its own biquad effect; apply_eq_preset takes per-band
# indices so the same map is reused for all three bands.
const _BIQUAD_PARAM_ID: Dictionary = {
	"cutoff_hz":       2,    # Biquad_CutoffHz
	"q":               3,    # Biquad_Q
	"biquad_gain_db": 12,    # Biquad_GainDb (used by shelves and peaks)
}

# JSON parameter name → EffectParameter ID for saturation.
const _SATURATION_PARAM_ID: Dictionary = {
	"drive":        19,    # Saturation_Drive
	"mix":          20,    # Saturation_Mix
	"output_gain":  21,    # Saturation_OutputGain
	"bias":         22,    # Saturation_Bias
	"mode":         27,    # Saturation_Mode (int)
}
